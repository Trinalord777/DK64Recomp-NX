// RT64 "extended GBI" (opcode 0x64) for GLideN64. DK64: Recompiled's patches emit these commands
// because RT64 is the PC renderer. GLideN64 must at least skip their data words (1 to 3 commands
// long), otherwise those words would be decoded as RDP commands. Only what changes the 4:3 image
// is implemented; interpolation and widescreen hints are skipped.
// Encodings: lib/rt64/include/rt64_extended_gbi.h. Semantics: lib/rt64/src/gbi/rt64_gbi_extended.cpp.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "CRC.h"
#include "Combiner.h"
#include "Config.h"
#include "Graphics/Context.h"
#include "Graphics/OpenGLContext/GLFunctions.h"
#include "Graphics/Parameters.h"
#include "FrameBuffer.h"
#include "GBI.h"
#include "N64.h"
#include "VI.h"
#include "RSP.h"
#include "gDP.h"
#include "gSP.h"

bool g_rt64ExtendedRDRAM = false;
// Alignment state for the 16:9 adjust mode (see RT64EX_AdjustRectX).
bool g_rt64ProjStretch = false;
bool g_rt64ScissorWide = false;
bool g_rt64ScissorPlain = false;  // last scissor was gEXSetScissor without origins
u16 g_rt64ViewportOrigin = 0x800;
static u16 g_rt64RectLeftOrigin = 0x800;
static u16 g_rt64RectRightOrigin = 0x800;
static u8 g_rt64RectAspect = 0;

static void interp_note_matrix_group(u32 flags);  // frame interpolation, below
static void interp_push_matrix_group(u32 id, u32 flags);
static void interp_pop_matrix_group(u32 count, bool projection);

namespace {
    constexpr u32 RT64_EXTENDED_OPCODE = 0x64;
    constexpr u32 G_EX_ORIGIN_NONE = 0x800;
    constexpr u32 G_EX_ORIGIN_RIGHT = 0x400;
    constexpr u32 G_EX_ASPECT_STRETCH = 0x1;
    constexpr u32 G_EX_COMPONENT_SKIP = 0x0;
    constexpr u32 G_EX_ASPECT_ADJUST = 0x2;

    enum ExOpcode : u32 {
        G_EX_FILLRECT_V1 = 0x03,
        G_EX_SETSCISSOR_V1 = 0x05,
        G_EX_SETRECTALIGN_V1 = 0x06,
        G_EX_SETVIEWPORTALIGN_V1 = 0x07,
        G_EX_MATRIXGROUP_V1 = 0x0C,
        G_EX_POPMATRIXGROUP_V1 = 0x0D,
        G_EX_PUSHVIEWPORT_V1 = 0x15,
        G_EX_POPVIEWPORT_V1 = 0x16,
        G_EX_PUSHSCISSOR_V1 = 0x17,
        G_EX_POPSCISSOR_V1 = 0x18,
        G_EX_SETRDRAMEXTENDED_V1 = 0x2C,
        G_EX_SETRECTASPECT_V1 = 0x33,
        G_EX_MAX = 0x34,
    };

    // Commands (8 bytes each) per sub-opcode, checked against the G_EX_COMMANDn of every macro
    // and the words each RT64 handler consumes.
    constexpr u8 COMMAND_LENGTH[G_EX_MAX] = {
        1, 1, 3, 2, 2, 2, 2, 2, 3, 1, 1, 1, 2, 1, 1, 1,  // 0x00-0x0F
        1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x10-0x1F
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  // 0x20-0x2F
        2, 2, 1, 1,                                      // 0x30-0x33
    };

    std::vector<gSPInfo::Viewport> viewport_stack;
    std::vector<gDPScissor> scissor_stack;
    std::vector<std::pair<bool, bool>> scissor_flag_stack;  // wide, plain

    // The extended state carries from one display list to the next, as the N64's does. At 60 fps a list runs twice,
    // and the second pass would start with what the first left instead of what it started with; both passes now start
    // from the same state, so that only the interpolated matrices tell their images apart (checked on the console:
    // with interpolation off, the second image equals the first, mean difference 0.00 over 10 DK TV frames).
    struct ExtendedState {
        bool extended_rdram, proj_stretch, scissor_wide, scissor_plain;
        u16 viewport_origin, rect_left_origin, rect_right_origin;
        u8 rect_aspect;
        std::vector<gSPInfo::Viewport> viewports;
        std::vector<gDPScissor> scissors;
        std::vector<std::pair<bool, bool>> scissor_flags;
    };
    ExtendedState first_pass_state;
    // GLideN64's RSP and RDP state (segments, viewport, matrices, tiles, render modes) and TMEM carry over too.
    gSPInfo first_pass_gsp;
    gDPInfo first_pass_gdp;
    u64 first_pass_tmem[512];

    // Word `index` (0 = w0) of the extra command `extra` (1-based) following the current one.
    u32 extra_word(u32 extra, u32 index) {
        return *(u32*)&RDRAM[RSP.PC[RSP.PCi] + 8 * (extra - 1) + 4 * index];
    }

    // Same origin rule as RT64's RDP::movedFromOrigin, in 10.2 fixed point.
    s32 moved_from_origin(s32 x, u32 origin) {
        if (origin >= G_EX_ORIGIN_NONE) {
            return x;
        }
        return x + static_cast<s32>((origin * gDP.colorImage.width * 4) / G_EX_ORIGIN_RIGHT);
    }

    void set_scissor(u32 w1) {
        const u32 mode = _SHIFTR(w1, 0, 2);
        const u32 left_origin = _SHIFTR(w1, 2, 12);
        const u32 right_origin = _SHIFTR(w1, 14, 12);
        const u32 ul = extra_word(1, 0);
        const u32 lr = extra_word(1, 1);
        const s32 ulx = moved_from_origin(static_cast<s16>(_SHIFTR(ul, 16, 16)), left_origin);
        const s32 uly = static_cast<s16>(_SHIFTR(ul, 0, 16));
        const s32 lrx = moved_from_origin(static_cast<s16>(_SHIFTR(lr, 16, 16)), right_origin);
        const s32 lry = static_cast<s16>(_SHIFTR(lr, 0, 16));
        gDPSetScissor(mode, ulx, uly, lrx, lry);
        g_rt64ScissorWide = left_origin < G_EX_ORIGIN_NONE || right_origin < G_EX_ORIGIN_NONE;
        g_rt64ScissorPlain = !g_rt64ScissorWide;
    }

    void fill_rect() {
        const u32 ul = extra_word(1, 0);
        const u32 lr = extra_word(1, 1);
        // Coordinates are 10.2 fixed point; the RDP fill rectangle works in whole pixels.
        gDPFillRectangle(_SHIFTR(ul, 16, 16) >> 2, _SHIFTR(ul, 0, 16) >> 2,
                         _SHIFTR(lr, 16, 16) >> 2, _SHIFTR(lr, 0, 16) >> 2);
    }

    void pop_viewport() {
        if (viewport_stack.empty()) return;
        gSP.viewport = viewport_stack.back();
        viewport_stack.pop_back();
        gSP.changed |= CHANGED_VIEWPORT;
    }

    void pop_scissor() {
        if (scissor_stack.empty()) return;
        const gDPScissor saved = scissor_stack.back();
        scissor_stack.pop_back();
        gDPSetScissor(saved.mode, saved.xh, saved.yh, saved.xl, saved.yl);
        g_rt64ScissorWide = scissor_flag_stack.back().first;
        g_rt64ScissorPlain = scissor_flag_stack.back().second;
        scissor_flag_stack.pop_back();
    }

    void rt64_extended(u32 w0, u32 w1) {
        const u32 op = _SHIFTR(w0, 0, 24);
        if (op >= G_EX_MAX) {
            return;
        }
        switch (op) {
            case G_EX_FILLRECT_V1: fill_rect(); break;
            case G_EX_SETSCISSOR_V1: set_scissor(w1); break;
            case G_EX_PUSHVIEWPORT_V1: viewport_stack.push_back(gSP.viewport); break;
            case G_EX_POPVIEWPORT_V1: pop_viewport(); break;
            case G_EX_PUSHSCISSOR_V1:
                scissor_stack.push_back(gDP.scissor);
                scissor_flag_stack.emplace_back(g_rt64ScissorWide, g_rt64ScissorPlain);
                break;
            case G_EX_SETRECTALIGN_V1:
                g_rt64RectLeftOrigin = _SHIFTR(w1, 0, 12);
                g_rt64RectRightOrigin = _SHIFTR(w1, 12, 12);
                break;
            case G_EX_SETVIEWPORTALIGN_V1: g_rt64ViewportOrigin = _SHIFTR(w1, 0, 12); break;
            case G_EX_SETRECTASPECT_V1: g_rt64RectAspect = _SHIFTR(w1, 0, 2); break;
            case G_EX_MATRIXGROUP_V1: {
                // Second command word: push(0) proj(1) ... aspect(20-21). The sky's projection asks for STRETCH.
                const u32 flags = extra_word(1, 0);
                if (_SHIFTR(flags, 1, 1) != 0) {
                    g_rt64ProjStretch = _SHIFTR(flags, 20, 2) == G_EX_ASPECT_STRETCH;
                }
                interp_note_matrix_group(flags);
                interp_push_matrix_group(w1, flags);
                break;
            }
            case G_EX_POPMATRIXGROUP_V1: interp_pop_matrix_group(_SHIFTR(w1, 0, 8), _SHIFTR(w1, 8, 1) != 0); break;
            case G_EX_POPSCISSOR_V1: pop_scissor(); break;
            case G_EX_SETRDRAMEXTENDED_V1: g_rt64ExtendedRDRAM = _SHIFTR(w1, 0, 1) != 0; break;
            default: break;
        }
        RSP.PC[RSP.PCi] += 8 * (COMMAND_LENGTH[op] - 1);
    }
}

void RT64EX_Init() {
    GBI.cmd[RT64_EXTENDED_OPCODE] = rt64_extended;
}

// RT64's RSP::fromSegmented then maskPhysicalAddress: with extended RDRAM on, KSEG0 addresses
// (0x80000000+) skip the segment table and may point past 16MB, into the recomp heap.
u32 RSP_SegmentToPhysicalEx(u32 segaddr) {
    constexpr u32 KSEG0 = 0x80000000U;
    u32 address = (g_rt64ExtendedRDRAM && (segaddr & KSEG0) != 0)
        ? segaddr
        : gSP.segment[(segaddr >> 24) & 0x0F] + (segaddr & 0x00FFFFFF);
    address = (g_rt64ExtendedRDRAM && (address & KSEG0) != 0) ? address - KSEG0 : (address & 0x00FFFFFF);
    return address & RDRAMSize;
}


// DK64 starts its next frame in the buffer still on screen and relies on the RDP freeze (set at each
// swap, cleared once the VI has moved off that buffer) to hold the drawing back. The runtime does not
// freeze the RDP, so GLideN64 draws frame N+2 into the front buffer before frame N+1 is shown.
// GLideN64 clears m_changed on the buffer it presents, so a set flag on the VI origin means exactly that.
bool dk64nx_front_buffer_redrawn(u32 origin) {
    const FrameBuffer* buffer = frameBufferList().findBuffer(origin & 0xFFFFFF);
    return buffer != nullptr && buffer->m_changed;
}


// RT64 origin in N64 pixels: left edge, centre or right edge of the frame; no origin means centre.
f32 RT64EX_OriginX(u16 origin, f32 width) {
    return origin < G_EX_ORIGIN_NONE ? width * origin / G_EX_ORIGIN_RIGHT : width / 2.0f;
}

// RT64's rule (framebuffer renderer): full-width rects stretch with the screen, anchored or narrow
// ones keep their aspect; an explicit rect aspect overrides both.
bool RT64EX_ShouldAdjustRect(bool narrow) {
    if (g_rt64RectAspect == G_EX_ASPECT_ADJUST) return true;
    if (g_rt64RectAspect == G_EX_ASPECT_STRETCH) return false;
    // ponytail: DK64R only sets an origin-less RT64 scissor around the main menu's tiled background,
    // whose 32px tiles would otherwise be pulled to the centre one by one; revisit if another user appears.
    if (g_rt64ScissorPlain && g_rt64RectLeftOrigin >= G_EX_ORIGIN_NONE) return false;
    return narrow || g_rt64RectLeftOrigin < G_EX_ORIGIN_NONE || g_rt64RectRightOrigin < G_EX_ORIGIN_NONE;
}

// x' = O + (x - O) * scale, the left edge around the left origin and the right edge around the right
// one, so HUD anchored left or right sits against the 16:9 screen edge (DK64R "HUD Expand").
void RT64EX_AdjustRectX(f32* xs, u32 count, f32 scale, f32 width) {
    f32 min_x = xs[0];
    for (u32 i = 1; i < count; ++i) min_x = xs[i] < min_x ? xs[i] : min_x;
    const f32 left = RT64EX_OriginX(g_rt64RectLeftOrigin, width);
    const f32 right = RT64EX_OriginX(g_rt64RectRightOrigin, width);
    for (u32 i = 0; i < count; ++i) {
        const f32 origin = xs[i] <= min_x ? left : right;
        xs[i] = origin + (xs[i] - origin) * scale;
    }
}

// Changes a GLideN64 setting while the game runs: the frame rate option (core_switch.cpp) and the developer
// harness ("gl" command, to measure what a setting costs). Graphics thread for the former. Returns the previous
// value, or -1 for an unknown name.
// A projection (camera) matrix that changes more than this between two frames is a camera cut, which DK64R's patches
// do not always mark (the DK Rap's shots): that frame gets no in-between image. Measured on the console (largest
// element change per frame): turning the camera at full speed while running stays under 0.1 in rotation but reaches
// 200-1000 in translation, which mixes the camera's position and rotation; the DK Rap's unmarked cuts go past 0.25 in
// rotation. A translation of 200 counted 4 false cuts in 4 s of play (one frame at 30 fps each time). The dev
// harness tunes them (gl cutrot <hundredths>, gl cuttrans <units>).
static f32 cut_rotation = 0.25f;
static f32 cut_translation = 5000.0f;

// A frame whose first pass took longer than this gets no in-between image: both passes and the presents must fit in
// the game's 33.3 ms frame, or the game itself loses a frame (a 50-60 ms freeze); the second pass costs about as much
// as the first. Measured in Creepy Castle at 1020 MHz, 3x, over 199 frames: no limit lost 34-49 frames, 17 ms lost 26
// (75% with an in-between), 15 ms lost 2 (68%), 11-13 ms lost 2 (50%). Dev harness: gl pass2max <microseconds>.
static u32 pass2_max_us = 15000;

// Dev harness experiment (gl lerpmask): which matrices the second pass interpolates. 1: paired by group ID,
// 2: paired by rank and likeness, 4: projections. 0 makes the in-between image a plain second render of the frame.
static u32 lerp_mask = 7;

// Graphics thread: GLideN64 saves its shaders storage (the combiners seen so far, compiled again at the next start) only
// at shutdown, which the game closed from the HOME menu never reaches: every session then compiled each new effect in
// the middle of play, a freeze of up to 122 ms measured. The swap hook calls this every few seconds.
extern "C" void dk64nx_gliden64_save_shaders() {
    CombinerInfo::get().saveShadersStorageIfChanged();
}

extern "C" int dk64nx_gliden64_set(const char* name, int value) {
    if (strcmp(name, "cutrot") == 0) {
        const int previous = static_cast<int>(cut_rotation * 100.0f);
        cut_rotation = value / 100.0f;
        return previous;
    }
    if (strcmp(name, "cuttrans") == 0) {
        const int previous = static_cast<int>(cut_translation);
        cut_translation = static_cast<f32>(value);
        return previous;
    }
    if (strcmp(name, "lerpmask") == 0) {
        const int previous = static_cast<int>(lerp_mask);
        lerp_mask = static_cast<u32>(value);
        return previous;
    }
    if (strcmp(name, "pass2max") == 0) {
        const int previous = static_cast<int>(pass2_max_us);
        pass2_max_us = static_cast<u32>(value);
        return previous;
    }
    struct Field { const char* name; u32* value; };
    const Field fields[] = {
        {"copy", &config.frameBufferEmulation.copyToRDRAM},
        {"aux", &config.frameBufferEmulation.copyAuxToRDRAM},
        {"fromrdram", &config.frameBufferEmulation.copyFromRDRAM},
        {"lod", &config.generalEmulation.enableLOD},
    };
    for (const Field& field : fields) {
        if (strcmp(name, field.name) == 0) {
            const int previous = static_cast<int>(*field.value);
            *field.value = static_cast<u32>(value);
            return previous;
        }
    }
    return -1;
}

// Frame interpolation (the NX Options frame rate at 60): the game still runs at 30 fps, and each display list is
// processed twice. The first pass is the real frame and records every matrix gSPMatrix loads. The second pass loads,
// rank by rank, the average of that matrix and the previous frame's matrix of the same rank, when both are close
// enough to be the same object (measured on the console: ~97% of them are), and keeps the real one otherwise. Its
// result is the in-between frame, shown one VI before the real one (render_gliden64.cpp). DK64R's patches mark camera
// cuts with a projection matrix group whose position does not interpolate: those frames get no in-between image.
namespace {
    // How the second pass finds a matrix's twin, from DK64R's RT64 matrix groups (gEXMatrixGroup), as RT64 does on PC
    // (rt64_game_frame.cpp buildTransformIdMap): a group with an explicit ID and linear ordering pairs its k-th matrix
    // with the previous frame's k-th matrix of the same ID; an ignored group is not interpolated; the rest (no group,
    // automatic IDs) is matched by rank and likeness.
    enum class Pairing : u8 { Heuristic, ById, Ignored };

    struct LoadedMatrix {
        u32 address;
        u8 param;
        f32 m[4][4];
        Pairing pairing = Pairing::Heuristic;
        u32 group = 0;
        u32 group_index = 0;
        u32 group_flags = 0;  // components to interpolate (G_EX_COMPONENT_* fields of the group's second word)
    };
    std::vector<LoadedMatrix> matrices_current;
    std::vector<LoadedMatrix> matrices_previous;

    constexpr u32 G_EX_ID_IGNORE = 0x0;
    constexpr u32 G_EX_ID_AUTO = 0xFFFFFFFF;
    constexpr u32 G_EX_ORDER_LINEAR = 0x0;
    constexpr size_t GROUP_STACK_LIMIT = 64;
    struct MatrixGroup { bool set; u32 id; u32 flags; };
    // Groups in effect while a display list runs, as RT64's model and view-projection matrix ID stacks; reset at the
    // start of each pass, so both passes see the same groups.
    std::vector<MatrixGroup> model_groups{{false, 0, 0}};
    std::vector<MatrixGroup> projection_groups{{false, 0, 0}};
    std::map<u32, u32> group_counts;               // first pass: matrices loaded so far per group ID
    std::map<u64, size_t> previous_by_group;        // previous frame: (ID, index) -> rank

    void reset_matrix_groups() {
        model_groups.assign(1, {false, 0, 0});
        projection_groups.assign(1, {false, 0, 0});
    }

    u64 group_key(u32 id, u32 index) { return (static_cast<u64>(id) << 32) | index; }

    constexpr f32 CLOSE_TRANSLATION = 30.0f;  // world units the same object may move in one frame
    constexpr f32 CLOSE_ROTATION = 0.3f;      // largest change of a rotation/scale element
    // A matrix looks for its twin this many ranks around the expected one: an object appearing or disappearing shifts
    // every rank after it, and the search follows that shift.
    constexpr size_t MATCH_WINDOW = 8;
    // Histogram edges of the largest projection change per frame (mtx stats), to tell camera cuts from fast turns.
    constexpr f32 ROTATION_EDGES[] = {0.05f, 0.1f, 0.25f, 0.5f, 1.0f};
    constexpr f32 TRANSLATION_EDGES[] = {50.0f, 200.0f, 1000.0f, 5000.0f, 20000.0f};
    constexpr size_t EDGE_COUNT = 5;

    size_t bucket(const f32 (&edges)[EDGE_COUNT], f32 value) {
        size_t i = 0;
        while (i < EDGE_COUNT && value >= edges[i]) i++;
        return i;
    }

    bool substituting = false;  // second pass: matrices are replaced by rank
    size_t frame_unmatched = 0;  // in this second pass
    // The second pass loaded other matrices, vertices or commands than the first: the game builds its next frame while
    // the list is processed, and rewrote part of what this one draws in between (the N64's RSP reads a list once,
    // early). Measured in DK TV: 3-9% of the frames while its logo and text show, none elsewhere. An in-between image
    // drawn from both versions would mix them, and substitutions after a matrix mismatch land on the wrong rank, so
    // the in-between image is dropped and the real one shows twice.
    bool pass2_diverged = false;
    // Vertex loads of the first pass (address, hash of the data), checked against the second pass's.
    struct VertexLoad { u32 address; u64 hash; };
    bool vertices_diverged_frame = false;
    // Every command each pass executed (FNV-1a of both words, and their count): a display list the game edited between
    // the passes (HUD sub-lists, rectangles, texture coordinates) shows here even when no matrix or vertex changed.
    constexpr u64 FNV_OFFSET = 0xcbf29ce484222325ULL;
    constexpr u64 FNV_PRIME = 0x100000001b3ULL;
    u64 command_hash = FNV_OFFSET;
    u64 command_count = 0;
    u64 first_pass_command_hash = 0;
    u64 first_pass_command_count = 0;
    std::vector<VertexLoad> vertices_current;
    size_t vertex_rank = 0;
    // Dev harness (ishotc / pairdump): how each matrix of the second pass was paired: method and twin rank.
    struct PairLog { char method; ptrdiff_t twin; };
    std::vector<PairLog> pair_log;
    bool frame_camera_unmatched = false;
    size_t substitute_rank = 0;
    ptrdiff_t rank_shift = 0;   // previous rank - current rank of the last match
    bool camera_cut = false;
    u32 saved_address = 0;      // color buffer of the frame in its second pass
    FrameBuffer* saved_buffer = nullptr;

    // In-between images waiting for the VI, one per color buffer (DK64 alternates two). A frame drawn into the
    // buffer still on screen (no RDP freeze) waits for the VI to leave that buffer and come back: shown at once,
    // it came before the previous frame's real image, a 75/25 step in motion.
    struct InBetween {
        FrameBuffer* buffer;
        bool wait_for_other_buffer;
    };
    std::map<u32, InBetween> in_between;
    FrameBuffer* in_between_shown = nullptr;
    u32 frames_begun = 0;
    std::map<u32, u32> frame_in_buffer;  // by color buffer address, for the dev harness

    struct MatchStats {
        u64 frames = 0;
        u64 matrices = 0;
        u64 equal = 0;
        u64 close = 0;
        u64 far = 0;
        u64 count_changed = 0;
        u64 interpolated = 0;
        u64 camera_cuts = 0;
        u64 over_budget = 0;    // frames whose first pass was too long for a second one
        u64 unmatched = 0;      // second-pass matrices left as they are
        u64 mostly_unmatched = 0;  // in-between images where most matrices were left as they are (no motion)
        u64 camera_unmatched = 0;  // in-between images whose projection (camera) matrix was left as it is
        u64 id_paired = 0;         // second-pass matrices paired by their RT64 group ID
        u64 id_missing = 0;        // ... whose ID and index were not in the previous frame
        u64 ignored = 0;           // ... in a group that asks for no interpolation
        u64 rank_disagree = 0;     // ID pairs where the rank and likeness search would have picked another matrix
        u64 identical_preferred = 0;  // rank searches where an identical matrix won over a nearer-ranked close one
        u64 pass2_diverged = 0;       // in-between images dropped: the second pass loaded other matrices or vertices
        u64 vertices_diverged = 0;    // ... of which the vertices differed
        u64 commands_diverged = 0;    // ... of which the commands differed
        u64 projection_rotation[EDGE_COUNT + 1] = {};
        u64 projection_translation[EDGE_COUNT + 1] = {};
    } match_stats;
    // Counts before the harness's current window (dk64nx_interp_stats resets match_stats), for NX Options' Dev Tools.
    struct { u64 frames = 0, interpolated = 0, camera_cuts = 0, unmatched = 0; } earlier_windows;
    std::mutex match_mutex;

    bool same_object(const LoadedMatrix& a, const LoadedMatrix& b) {
        if (a.param != b.param) return false;
        f32 translation = 0.0f;
        f32 rotation = 0.0f;
        for (int r = 0; r < 3; r++) {
            translation = std::max(translation, std::fabs(a.m[3][r] - b.m[3][r]));
            for (int c = 0; c < 3; c++) rotation = std::max(rotation, std::fabs(a.m[r][c] - b.m[r][c]));
        }
        return translation < CLOSE_TRANSLATION && rotation < CLOSE_ROTATION;
    }

    void compare_frames() {
        const size_t count = std::min(matrices_previous.size(), matrices_current.size());
        std::lock_guard lock{match_mutex};
        match_stats.frames++;
        match_stats.matrices += matrices_current.size();
        match_stats.count_changed += matrices_previous.size() != matrices_current.size();
        for (size_t i = 0; i < count; i++) {
            const LoadedMatrix& a = matrices_previous[i];
            const LoadedMatrix& b = matrices_current[i];
            if (a.param == b.param && memcmp(a.m, b.m, sizeof(a.m)) == 0) match_stats.equal++;
            else if (same_object(a, b)) match_stats.close++;
            else match_stats.far++;
        }
    }

    bool perspective(const LoadedMatrix& m) { return std::fabs(m.m[2][3]) > 0.5f && std::fabs(m.m[3][3]) < 0.5f; }

    // Largest change of the 3x3 part, of the translation row and of the projective column between two matrices.
    struct MatrixDelta { f32 linear = 0.0f, translation = 0.0f, projective = 0.0f; };
    MatrixDelta matrix_delta(const LoadedMatrix& a, const LoadedMatrix& b) {
        MatrixDelta d;
        for (int r = 0; r < 3; r++) {
            d.translation = std::max(d.translation, std::fabs(a.m[3][r] - b.m[3][r]));
            for (int c = 0; c < 3; c++) d.linear = std::max(d.linear, std::fabs(a.m[r][c] - b.m[r][c]));
        }
        for (int r = 0; r < 4; r++) d.projective = std::max(d.projective, std::fabs(a.m[r][3] - b.m[r][3]));
        return d;
    }

    // Two projection matrices that can be the same camera one frame apart: same load mode, both perspective or both
    // affine (ortho, look-at), and no larger change than a camera cut's. A frame's projections are not in a fixed
    // order: the HUD's orthographic matrix comes before the camera in some frames and after it in others, and the 3D
    // HUD icons have their own perspective camera (square aspect); paired by order (6e48917), the camera was averaged
    // with one of them on the frames where their number changed, and the in-between image exploded.
    //
    // A loaded projection (perspective or orthographic) must also keep its scale: its diagonal (field of view, aspect,
    // the ortho's 2/width) within 2%. The absolute limit let the HUD's 320x240 ortho (0.006) pair with its 640x480 one
    // (0.002) when DK TV's blinking text shifted their indices in the camera's group, and the DK64 logo drawn with the
    // average shrank toward the top left of every other second's in-between image.
    bool same_camera(const LoadedMatrix& a, const LoadedMatrix& b, const MatrixDelta& d) {
        if (a.param != b.param || perspective(a) != perspective(b) || d.linear >= cut_rotation ||
            d.translation >= cut_translation || d.projective >= 0.05f) {
            return false;
        }
        if (a.param & G_MTX_LOAD) {
            for (int i = 0; i < 3; i++) {
                const f32 x = a.m[i][i];
                const f32 y = b.m[i][i];
                if (std::fabs(x - y) > 0.02f * std::max(std::fabs(x), std::fabs(y)) + 1e-7f) return false;
            }
        }
        return true;
    }

    // The previous frame's projection most like this one among those that can be the same camera, or -1.
    ptrdiff_t projection_twin(size_t rank) {
        const LoadedMatrix& now = matrices_current[rank];
        ptrdiff_t best = -1;
        f32 best_score = 0.0f;
        for (size_t j = 0; j < matrices_previous.size(); j++) {
            const LoadedMatrix& before = matrices_previous[j];
            if (!(before.param & G_MTX_PROJECTION)) continue;
            const MatrixDelta d = matrix_delta(before, now);
            if (!same_camera(before, now, d)) continue;
            const f32 score = d.linear / cut_rotation + d.translation / cut_translation;
            if (best < 0 || score < best_score) {
                best = static_cast<ptrdiff_t>(j);
                best_score = score;
            }
        }
        return best;
    }

    // Dev harness (proj): the projection matrices of the last two frames, captured on request.
    std::atomic<bool> proj_dump_wanted{false};
    std::string proj_dump;

    void describe_projections(std::string& out, const std::vector<LoadedMatrix>& list) {
        char item[160];
        for (size_t i = 0; i < list.size(); i++) {
            const LoadedMatrix& m = list[i];
            if (!(m.param & G_MTX_PROJECTION)) continue;
            snprintf(item, sizeof(item), " [rank %zu %s%s %08x d=%.2f,%.2f,%.3f c3=%.2f,%.2f t=%.0f,%.0f,%.0f]", i,
                     (m.param & G_MTX_LOAD) ? "LOAD" : "MUL", perspective(m) ? " persp" : " affine", m.address,
                     m.m[0][0], m.m[1][1], m.m[2][2], m.m[2][3], m.m[3][3], m.m[3][0], m.m[3][1], m.m[3][2]);
            out += item;
        }
    }

    // The previous frame's twin of a matrix without a group ID, or -1. Projections: the camera's view matrix holds
    // -R * eye, which moves by more than an object's distance limit when the camera turns far from the map's origin
    // (131-159 of ~265 in-between images left it unmatched while running in Creepy Castle's basement), so they pair by
    // camera likeness (projection_twin). Other matrices: the expected rank first, then the nearest ranks outwards.
    // Load order, not likeness, decides: the DK Rap's five similar skeletons stand side by side, and a bone often looks
    // more like the neighbour's bone than like itself one frame earlier.
    ptrdiff_t find_twin(size_t rank) {
        const LoadedMatrix& now = matrices_current[rank];
        if (now.param & G_MTX_PROJECTION) return projection_twin(rank);
        const ptrdiff_t expected = static_cast<ptrdiff_t>(rank) + rank_shift;
        const ptrdiff_t count = static_cast<ptrdiff_t>(matrices_previous.size());
        ptrdiff_t nearest = -1;
        for (ptrdiff_t offset = 0; offset <= static_cast<ptrdiff_t>(MATCH_WINDOW); offset++) {
            for (const ptrdiff_t j : {expected - offset, expected + offset}) {
                if (j < 0 || j >= count) continue;
                const LoadedMatrix& before = matrices_previous[j];
                // An identical matrix is the same still object (a logo, the HUD, the scenery) and wins over a nearer
                // rank: when an element before it comes or goes, the ranks after it shift, and a nearer one only close
                // enough would move a still object (it decided 68-79 pairs per DK TV run).
                if (before.param == now.param && memcmp(before.m, now.m, sizeof(now.m)) == 0) {
                    if (nearest >= 0) {
                        std::lock_guard lock{match_mutex};
                        match_stats.identical_preferred++;
                    }
                    return j;
                }
                if (nearest < 0 && same_object(before, now)) nearest = j;
                if (offset == 0) break;
            }
        }
        return nearest;
    }

    // The main camera of a frame: its first perspective projection loaded, and the look-at multiplied onto it.
    struct MainCamera { ptrdiff_t projection = -1, look_at = -1; };
    MainCamera main_camera(const std::vector<LoadedMatrix>& list) {
        MainCamera camera;
        for (size_t i = 0; i < list.size(); i++) {
            const LoadedMatrix& m = list[i];
            if (!(m.param & G_MTX_PROJECTION)) continue;
            if (camera.projection < 0) {
                if ((m.param & G_MTX_LOAD) && perspective(m)) camera.projection = static_cast<ptrdiff_t>(i);
                continue;
            }
            if (!(m.param & G_MTX_LOAD)) camera.look_at = static_cast<ptrdiff_t>(i);
            break;
        }
        return camera;
    }

    // The main camera of both frames: one that jumped, appeared or disappeared is a camera cut. Projections other than
    // the camera's (HUD) come and go and are not compared: paired by load order, they counted 52 cuts in 349 frames
    // in Gloomy Galleon, each one a frame shown without its in-between image.
    bool camera_jumped() {
        const MainCamera before = main_camera(matrices_previous);
        const MainCamera now = main_camera(matrices_current);
        f32 translation = 0.0f;
        f32 rotation = 0.0f;
        bool appeared = (before.projection < 0) != (now.projection < 0);
        for (const auto& [a, b] : {std::pair{before.projection, now.projection}, std::pair{before.look_at, now.look_at}}) {
            if (a < 0 || b < 0) {
                appeared = appeared || (a < 0) != (b < 0);
                continue;
            }
            const MatrixDelta d = matrix_delta(matrices_previous[a], matrices_current[b]);
            translation = std::max(translation, d.translation);
            rotation = std::max(rotation, std::max(d.linear, d.projective));
        }
        std::lock_guard lock{match_mutex};
        match_stats.projection_rotation[bucket(ROTATION_EDGES, rotation)]++;
        match_stats.projection_translation[bucket(TRANSLATION_EDGES, translation)]++;
        return appeared || rotation > cut_rotation || translation > cut_translation;
    }

    FrameBuffer* saved_frame() {
        FrameBuffer* buffer = frameBufferList().findBuffer(saved_address);
        return buffer == saved_buffer ? buffer : nullptr;
    }
}

// First pass: the matrix group a matrix belongs to (the top of RT64's model or view-projection ID stack).
static void tag_matrix_group(LoadedMatrix& loaded) {
    const MatrixGroup& group = (loaded.param & G_MTX_PROJECTION) ? projection_groups.back() : model_groups.back();
    if (!group.set || group.id == G_EX_ID_AUTO) return;
    if (group.id == G_EX_ID_IGNORE) {  // whatever its ordering, as RT64 does
        loaded.pairing = Pairing::Ignored;
        return;
    }
    if (_SHIFTR(group.flags, 17, 2) != G_EX_ORDER_LINEAR) return;
    loaded.pairing = Pairing::ById;
    loaded.group = group.id;
    loaded.group_index = group_counts[group.id]++;
    loaded.group_flags = group.flags;
}

// Second pass, a matrix paired by ID: its twin of the previous frame, or -1. A projection must still be able to be the
// same camera: DK64R's camera group stays in effect after the camera (NOPUSH), so the HUD's projections that follow
// carry its ID, at an index that moves when their number changes.
static ptrdiff_t id_twin(const LoadedMatrix& now) {
    const auto found = previous_by_group.find(group_key(now.group, now.group_index));
    if (found == previous_by_group.end()) return -1;
    const LoadedMatrix& before = matrices_previous[found->second];
    if (before.param != now.param) return -1;
    if ((now.param & G_MTX_PROJECTION) && !same_camera(before, now, matrix_delta(before, now))) return -1;
    return static_cast<ptrdiff_t>(found->second);
}

void RT64EX_OnCommand(u32 w0, u32 w1) {
    command_hash = (command_hash ^ ((static_cast<u64>(w0) << 32) | w1)) * FNV_PRIME;
    command_count++;
}

void RT64EX_OnVertices(u32 address, u32 size) {
    const u64 hash = CRC_Calculate(0xFFFFFFFF, RDRAM + address, size);
    if (!substituting) {
        vertices_current.push_back({address, hash});
        return;
    }
    const size_t rank = vertex_rank++;
    if (rank >= vertices_current.size() || vertices_current[rank].address != address || vertices_current[rank].hash != hash) {
        pass2_diverged = true;
        vertices_diverged_frame = true;
    }
}

void RT64EX_OnMatrix(u32 address, u8 param, f32 mtx[4][4]) {
    if (!substituting) {
        LoadedMatrix loaded{address, param, {}};
        memcpy(loaded.m, mtx, sizeof(loaded.m));
        tag_matrix_group(loaded);
        matrices_current.push_back(loaded);
        return;
    }
    const size_t rank = substitute_rank++;
    pair_log.push_back({'D', -1});
    if (pass2_diverged) return;
    if (rank >= matrices_current.size()) {
        pass2_diverged = true;
        return;
    }
    const LoadedMatrix& now = matrices_current[rank];
    if (now.address != address || now.param != param || memcmp(now.m, mtx, sizeof(now.m)) != 0) {
        pass2_diverged = true;
        return;
    }
    if (now.pairing == Pairing::Ignored) {
        pair_log.back().method = 'i';
        std::lock_guard lock{match_mutex};
        match_stats.ignored++;
        return;
    }
    ptrdiff_t twin = -1;
    if (now.pairing == Pairing::ById) {
        twin = id_twin(now);
        const ptrdiff_t heuristic = find_twin(rank);  // what the rank and likeness search alone would pick
        std::lock_guard lock{match_mutex};
        if (twin < 0) match_stats.id_missing++;
        else match_stats.id_paired++;
        match_stats.rank_disagree += twin >= 0 && heuristic >= 0 && heuristic != twin;
    } else {
        twin = find_twin(rank);
    }
    pair_log.back() = {now.pairing == Pairing::ById ? 'I' : 'H', twin};
    if (twin < 0) {
        std::lock_guard lock{match_mutex};
        match_stats.unmatched++;
        frame_unmatched++;
        if (param & G_MTX_PROJECTION) frame_camera_unmatched = true;
        return;
    }
    rank_shift = twin - static_cast<ptrdiff_t>(rank);
    const u32 kind = (param & G_MTX_PROJECTION) ? 4u : (now.pairing == Pairing::ById ? 1u : 2u);
    if ((lerp_mask & kind) == 0) return;
    const LoadedMatrix& before = matrices_previous[twin];
    // Components the group leaves alone keep this frame's value (RT64: G_EX_COMPONENT_SKIP): the translation row
    // (position), the 3x3 part (rotation, scale, skew) and the projective column (perspective).
    const u32 flags = now.pairing == Pairing::ById ? now.group_flags : 0xFFFFFFFF;
    const bool position = _SHIFTR(flags, 3, 2) != G_EX_COMPONENT_SKIP;
    const bool linear = _SHIFTR(flags, 5, 2) != G_EX_COMPONENT_SKIP;
    const bool projective = _SHIFTR(flags, 11, 2) != G_EX_COMPONENT_SKIP;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            const bool interpolate = c == 3 ? projective : (r == 3 ? position : linear);
            mtx[r][c] = interpolate ? 0.5f * (before.m[r][c] + now.m[r][c]) : now.m[r][c];
        }
    }
}

static void interp_push_matrix_group(u32 id, u32 flags) {
    std::vector<MatrixGroup>& stack = _SHIFTR(flags, 1, 1) != 0 ? projection_groups : model_groups;
    const MatrixGroup group{true, id, flags};
    if (_SHIFTR(flags, 0, 1) != 0 && stack.size() < GROUP_STACK_LIMIT) stack.push_back(group);
    else stack.back() = group;
}

static void interp_pop_matrix_group(u32 count, bool projection) {
    std::vector<MatrixGroup>& stack = projection ? projection_groups : model_groups;
    while (count-- > 0 && stack.size() > 1) stack.pop_back();
}

// The projection matrix group of DK64R's camera (handle_interpolation in patches_interpolation.c).
static void interp_note_matrix_group(u32 flags) {
    const bool projection = _SHIFTR(flags, 1, 1) != 0;
    if (projection && _SHIFTR(flags, 3, 2) == G_EX_COMPONENT_SKIP) camera_cut = true;
}

// Graphics thread, before each display list.
extern "C" void dk64nx_interp_frame_begin() {
    if (!matrices_current.empty()) {
        compare_frames();
        std::swap(matrices_previous, matrices_current);
    }
    matrices_current.clear();
    vertices_current.clear();
    command_hash = FNV_OFFSET;
    command_count = 0;
    camera_cut = false;
    frames_begun++;
    previous_by_group.clear();
    for (size_t i = 0; i < matrices_previous.size(); i++) {
        const LoadedMatrix& m = matrices_previous[i];
        if (m.pairing == Pairing::ById) previous_by_group.emplace(group_key(m.group, m.group_index), i);
    }
    group_counts.clear();
    reset_matrix_groups();
}

// After the first pass, which took first_pass_us. False when this frame gets no in-between image.
extern "C" bool dk64nx_interp_begin_second_pass(u32 first_pass_us) {
    FrameBuffer* buffer = frameBufferList().getCurrent();
    if (buffer != nullptr) {
        in_between.erase(buffer->m_startAddress);  // that image belonged to an older frame
        frame_in_buffer[buffer->m_startAddress] = frames_begun;
    }
    if (proj_dump_wanted.exchange(false)) {
        std::string text = "previous:";
        describe_projections(text, matrices_previous);
        text += " | current:";
        describe_projections(text, matrices_current);
        std::lock_guard lock{match_mutex};
        proj_dump = text;
    }
    const bool jumped = camera_jumped();  // always measured, for the histograms
    if (camera_cut || jumped) {
        std::lock_guard lock{match_mutex};
        match_stats.camera_cuts++;
        return false;
    }
    if (first_pass_us > pass2_max_us) {
        std::lock_guard lock{match_mutex};
        match_stats.over_budget++;
        return false;
    }
    if (matrices_previous.empty() || buffer == nullptr || config.video.multisampling != 0) return false;
    buffer->interpCopy(0, true);
    saved_buffer = buffer;
    saved_address = buffer->m_startAddress;
    substituting = true;
    reset_matrix_groups();
    pass2_diverged = false;
    first_pass_command_hash = command_hash;
    first_pass_command_count = command_count;
    command_hash = FNV_OFFSET;
    command_count = 0;
    vertex_rank = 0;
    vertices_diverged_frame = false;
    pair_log.clear();
    frame_unmatched = 0;
    frame_camera_unmatched = false;
    substitute_rank = 0;
    rank_shift = 0;
    return true;
}

// Developer harness ("ishot"): both copies of the next interpolated frame, real then in-between, bottom-up RGBA.
namespace {
    std::mutex capture_mutex;
    std::condition_variable capture_cv;
    bool capture_wanted = false;
    bool capture_on_change = false;  // only a frame whose matrix count differs from the previous one's
    std::string pair_dump;

    void dump_pairs() {
        char line[200];
        std::string out;
        snprintf(line, sizeof(line), "previous %zu current %zu diverged %d\n", matrices_previous.size(),
                 matrices_current.size(), pass2_diverged ? 1 : 0);
        out += line;
        for (size_t i = 0; i < matrices_current.size(); i++) {
            const LoadedMatrix& m = matrices_current[i];
            const PairLog log = i < pair_log.size() ? pair_log[i] : PairLog{'-', -1};
            snprintf(line, sizeof(line), "%zu %c twin %td %08x p%02x g%08x/%u d %.3f %.3f %.3f t %.1f %.1f %.1f", i,
                     log.method, log.twin, m.address, m.param, m.group, m.group_index, m.m[0][0], m.m[1][1],
                     m.m[2][2], m.m[3][0], m.m[3][1], m.m[3][2]);
            out += line;
            if (log.twin >= 0 && static_cast<size_t>(log.twin) < matrices_previous.size()) {
                const LoadedMatrix& b = matrices_previous[log.twin];
                snprintf(line, sizeof(line), " | was %08x d %.3f %.3f %.3f t %.1f %.1f %.1f", b.address, b.m[0][0],
                         b.m[1][1], b.m[2][2], b.m[3][0], b.m[3][1], b.m[3][2]);
                out += line;
            }
            out += "\n";
        }
        out += "previous list:\n";
        for (size_t i = 0; i < matrices_previous.size(); i++) {
            const LoadedMatrix& m = matrices_previous[i];
            snprintf(line, sizeof(line), "%zu %08x p%02x g%08x/%u d %.3f %.3f %.3f t %.1f %.1f %.1f\n", i, m.address,
                     m.param, m.group, m.group_index, m.m[0][0], m.m[1][1], m.m[2][2], m.m[3][0], m.m[3][1], m.m[3][2]);
            out += line;
        }
        pair_dump = out;
    }
    bool capture_ready = false;
    std::vector<u8> capture_pixels[2];
    u32 capture_width = 0;
    u32 capture_height = 0;

    void capture_copies(FrameBuffer* buffer) {
        std::lock_guard lock{capture_mutex};
        if (!capture_wanted) return;
        if (capture_on_change && matrices_current.size() == matrices_previous.size()) return;
        dump_pairs();
        capture_width = buffer->m_pTexture->width;
        capture_height = buffer->m_pTexture->height;
        for (u32 slot = 0; slot < 2; slot++) {
            capture_pixels[slot].resize(size_t(capture_width) * capture_height * 4);
            gfxContext.bindFramebuffer(graphics::bufferTarget::READ_FRAMEBUFFER, buffer->m_interpFBO[slot]);
            glReadPixels(0, 0, capture_width, capture_height, GL_RGBA, GL_UNSIGNED_BYTE, capture_pixels[slot].data());
        }
        gfxContext.bindFramebuffer(graphics::bufferTarget::READ_FRAMEBUFFER, graphics::ObjectHandle::defaultFramebuffer);
        capture_wanted = false;
        capture_ready = true;
        capture_cv.notify_all();
    }
}

extern "C" bool dk64nx_interp_capture(u32* width, u32* height, const u8** real, const u8** in_between, bool on_change) {
    std::unique_lock lock{capture_mutex};
    capture_ready = false;
    capture_wanted = true;
    capture_on_change = on_change;
    if (!capture_cv.wait_for(lock, std::chrono::seconds(on_change ? 8 : 2), [] { return capture_ready; })) {
        capture_wanted = false;
        return false;
    }
    *width = capture_width;
    *height = capture_height;
    *real = capture_pixels[0].data();
    *in_between = capture_pixels[1].data();
    return true;
}

extern "C" void dk64nx_interp_end_second_pass() {
    substituting = false;
    if (FrameBuffer* buffer = saved_frame()) {
        buffer->interpCopy(1, true);   // the in-between frame
        capture_copies(buffer);
        buffer->interpCopy(0, false);  // the real one back
        const bool vertices_differ = vertex_rank != vertices_current.size();
        const bool commands_differ = command_hash != first_pass_command_hash || command_count != first_pass_command_count;
        if (pass2_diverged || vertices_differ || commands_differ || substitute_rank != matrices_current.size()) {
            std::lock_guard lock{match_mutex};
            match_stats.pass2_diverged++;
            match_stats.vertices_diverged += vertices_differ || vertices_diverged_frame;
            match_stats.commands_diverged += commands_differ;
            return;
        }
        in_between[buffer->m_startAddress] = {buffer, frameBufferList().findBuffer(*REG.VI_ORIGIN) == buffer};
        std::lock_guard lock{match_mutex};
        match_stats.interpolated++;
        match_stats.mostly_unmatched += frame_unmatched * 2 > matrices_current.size();
        match_stats.camera_unmatched += frame_camera_unmatched;
    }
}

// VI: when the frame about to be shown has an in-between image, put it in the color buffer and return true; the
// caller presents it, then calls dk64nx_interp_present_done to put the real frame back for the next VI.
extern "C" bool dk64nx_interp_present_in_between(u32 origin) {
    FrameBuffer* shown = frameBufferList().findBuffer(origin);
    for (auto& [address, pending] : in_between) {
        if (pending.buffer != shown) pending.wait_for_other_buffer = false;
    }
    if (shown == nullptr) return false;
    // Until the VI shows that frame's buffer: the swap may land a VI later than the display list.
    const auto pending = in_between.find(shown->m_startAddress);
    if (pending == in_between.end() || pending->second.wait_for_other_buffer) return false;
    const bool same_buffer = pending->second.buffer == shown;  // not recreated at that address since
    in_between.erase(pending);
    if (!same_buffer) return false;
    shown->interpCopy(1, false);
    in_between_shown = shown;
    return true;
}

// Dev harness: the color buffer the last display list drew into, 0 when none.
extern "C" u32 dk64nx_interp_current_buffer() {
    const FrameBuffer* buffer = frameBufferList().getCurrent();
    return buffer == nullptr ? 0 : buffer->m_startAddress;
}

// Dev harness: the position of the image the VI presents from origin, in half frames (in-between of frame n: 2n - 1).
extern "C" u32 dk64nx_interp_shown_position(u32 origin, bool in_between_image) {
    const FrameBuffer* buffer = frameBufferList().findBuffer(origin);
    if (buffer == nullptr) return 0;
    const auto frame = frame_in_buffer.find(buffer->m_startAddress);
    return frame == frame_in_buffer.end() ? 0 : 2 * frame->second - (in_between_image ? 1 : 0);
}

extern "C" void dk64nx_interp_present_done() {
    if (in_between_shown != nullptr) in_between_shown->interpCopy(0, false);
    in_between_shown = nullptr;
}

// Since the start: frames, frames with an in-between image, camera cuts, matrices left unmatched in second passes.
extern "C" void dk64nx_interp_totals(u64 out[4]) {
    std::lock_guard lock{match_mutex};
    out[0] = earlier_windows.frames + match_stats.frames;
    out[1] = earlier_windows.interpolated + match_stats.interpolated;
    out[2] = earlier_windows.camera_cuts + match_stats.camera_cuts;
    out[3] = earlier_windows.unmatched + match_stats.unmatched;
}

extern "C" int dk64nx_interp_stats(char* out, int size) {
    std::lock_guard lock{match_mutex};
    const MatchStats s = match_stats;
    earlier_windows.frames += s.frames;
    earlier_windows.interpolated += s.interpolated;
    earlier_windows.camera_cuts += s.camera_cuts;
    earlier_windows.unmatched += s.unmatched;
    match_stats = {};
    const double pairs = static_cast<double>(s.equal + s.close + s.far);
    char rotation[96];
    char translation[96];
    snprintf(rotation, sizeof(rotation), "%llu/%llu/%llu/%llu/%llu/%llu",
             (unsigned long long)s.projection_rotation[0], (unsigned long long)s.projection_rotation[1],
             (unsigned long long)s.projection_rotation[2], (unsigned long long)s.projection_rotation[3],
             (unsigned long long)s.projection_rotation[4], (unsigned long long)s.projection_rotation[5]);
    snprintf(translation, sizeof(translation), "%llu/%llu/%llu/%llu/%llu/%llu",
             (unsigned long long)s.projection_translation[0], (unsigned long long)s.projection_translation[1],
             (unsigned long long)s.projection_translation[2], (unsigned long long)s.projection_translation[3],
             (unsigned long long)s.projection_translation[4], (unsigned long long)s.projection_translation[5]);
    return snprintf(out, size, "frames=%llu interpolated=%llu cuts=%llu over_budget=%llu mostly_unmatched=%llu camera_unmatched=%llu id_paired=%llu id_missing=%llu ignored=%llu rank_disagree=%llu identical_preferred=%llu pass2_diverged=%llu vertices_diverged=%llu commands_diverged=%llu unmatched/frame=%.1f matrices/frame=%.0f equal=%.1f%% close=%.1f%% far=%.1f%% count_changed=%llu proj_rot(<.05/.1/.25/.5/1/more)=%s proj_trans(<50/200/1k/5k/20k/more)=%s",
                    (unsigned long long)s.frames, (unsigned long long)s.interpolated, (unsigned long long)s.camera_cuts,
                    (unsigned long long)s.over_budget, (unsigned long long)s.mostly_unmatched, (unsigned long long)s.camera_unmatched, (unsigned long long)s.id_paired, (unsigned long long)s.id_missing,
                    (unsigned long long)s.ignored, (unsigned long long)s.rank_disagree,
                    (unsigned long long)s.identical_preferred, (unsigned long long)s.pass2_diverged,
                    (unsigned long long)s.vertices_diverged, (unsigned long long)s.commands_diverged, s.interpolated ? double(s.unmatched) / s.interpolated : 0.0,
                    s.frames ? double(s.matrices) / s.frames : 0.0,
                    pairs ? 100.0 * s.equal / pairs : 0.0, pairs ? 100.0 * s.close / pairs : 0.0,
                    pairs ? 100.0 * s.far / pairs : 0.0, (unsigned long long)s.count_changed, rotation, translation);
}

// Dev harness: ask for the projection matrices of the next frame, then read them (empty until a frame came).
extern "C" void dk64nx_interp_request_projections() {
    {
        std::lock_guard lock{match_mutex};
        proj_dump.clear();
    }
    proj_dump_wanted = true;
}

extern "C" int dk64nx_interp_projections(char* out, int size) {
    std::lock_guard lock{match_mutex};
    return snprintf(out, size, "%s", proj_dump.c_str());
}

// Graphics thread: the extended state before a display list's first pass, and again before its second pass.
extern "C" void dk64nx_rt64ex_save_state() {
    first_pass_gsp = gSP;
    first_pass_gdp = gDP;
    memcpy(first_pass_tmem, TMEM, sizeof(first_pass_tmem));
    first_pass_state = {g_rt64ExtendedRDRAM, g_rt64ProjStretch, g_rt64ScissorWide, g_rt64ScissorPlain,
                        g_rt64ViewportOrigin, g_rt64RectLeftOrigin, g_rt64RectRightOrigin, g_rt64RectAspect,
                        viewport_stack, scissor_stack, scissor_flag_stack};
}

extern "C" void dk64nx_rt64ex_restore_state() {
    // The color and depth images stay: GLideN64 keeps its current frame buffer with them, and DK64 sets both first.
    const auto color_image = gDP.colorImage;
    const u32 depth_image = gDP.depthImageAddress;
    const u32 gdp_changed = gDP.changed;
    const u32 gsp_changed = gSP.changed;
    gSP = first_pass_gsp;
    gDP = first_pass_gdp;
    gDP.colorImage = color_image;
    gDP.depthImageAddress = depth_image;
    memcpy(TMEM, first_pass_tmem, sizeof(first_pass_tmem));
    // Everything that reaches the GL state is applied again; never a frame buffer reload from RDRAM.
    gSP.changed = gsp_changed | CHANGED_VIEWPORT | CHANGED_MATRIX | CHANGED_TEXTURE | CHANGED_GEOMETRYMODE |
                  CHANGED_FOGPOSITION | CHANGED_LIGHT | CHANGED_LOOKAT | CHANGED_TEXTURESCALE;
    gDP.changed = gdp_changed | CHANGED_RENDERMODE | CHANGED_CYCLETYPE | CHANGED_SCISSOR | CHANGED_TMEM | CHANGED_TILE |
                  CHANGED_COMBINE | CHANGED_ALPHACOMPARE | CHANGED_FOGCOLOR | CHANGED_BLENDCOLOR;
    const ExtendedState& state = first_pass_state;
    g_rt64ExtendedRDRAM = state.extended_rdram;
    g_rt64ProjStretch = state.proj_stretch;
    g_rt64ScissorWide = state.scissor_wide;
    g_rt64ScissorPlain = state.scissor_plain;
    g_rt64ViewportOrigin = state.viewport_origin;
    g_rt64RectLeftOrigin = state.rect_left_origin;
    g_rt64RectRightOrigin = state.rect_right_origin;
    g_rt64RectAspect = state.rect_aspect;
    viewport_stack = state.viewports;
    scissor_stack = state.scissors;
    scissor_flag_stack = state.scissor_flags;
}

// Dev harness: the pairing report of the last captured frame.
extern "C" int dk64nx_interp_pair_dump(char* out, int size) {
    std::lock_guard lock{capture_mutex};
    return snprintf(out, size, "%s", pair_dump.c_str());
}
