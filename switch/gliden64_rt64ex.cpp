// RT64 "extended GBI" (opcode 0x64) for GLideN64. DK64: Recompiled's patches emit these commands
// because RT64 is the PC renderer. GLideN64 must at least skip their data words (1 to 3 commands
// long), otherwise those words would be decoded as RDP commands. Only what changes the 4:3 image
// is implemented; interpolation and widescreen hints are skipped.
// Encodings: lib/rt64/include/rt64_extended_gbi.h. Semantics: lib/rt64/src/gbi/rt64_gbi_extended.cpp.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "Config.h"
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
                break;
            }
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
extern "C" int dk64nx_gliden64_set(const char* name, int value) {
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
    struct LoadedMatrix {
        u32 address;
        u8 param;
        f32 m[4][4];
    };
    std::vector<LoadedMatrix> matrices_current;
    std::vector<LoadedMatrix> matrices_previous;

    constexpr f32 CLOSE_TRANSLATION = 50.0f;  // world units the same object may move in one frame
    constexpr f32 CLOSE_ROTATION = 0.3f;      // largest change of a rotation/scale element
    // A matrix looks for its twin this many ranks around the expected one: an object appearing or disappearing shifts
    // every rank after it, and the search follows that shift.
    constexpr size_t MATCH_WINDOW = 8;

    bool substituting = false;  // second pass: matrices are replaced by rank
    size_t substitute_rank = 0;
    ptrdiff_t rank_shift = 0;   // previous rank - current rank of the last match
    bool camera_cut = false;
    u32 saved_address = 0;      // color buffer holding the two copies of the last interpolated frame
    FrameBuffer* saved_buffer = nullptr;
    bool in_between_pending = false;

    struct MatchStats {
        u64 frames = 0;
        u64 matrices = 0;
        u64 equal = 0;
        u64 close = 0;
        u64 far = 0;
        u64 count_changed = 0;
        u64 interpolated = 0;
        u64 camera_cuts = 0;
        u64 unmatched = 0;      // second-pass matrices left as they are
    } match_stats;
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

    f32 distance(const LoadedMatrix& a, const LoadedMatrix& b) {
        f32 total = 0.0f;
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) total += std::fabs(a.m[r][c] - b.m[r][c]);
        }
        return total;
    }

    // The previous frame's twin of the current frame's matrix of this rank, or -1.
    ptrdiff_t find_twin(size_t rank) {
        const LoadedMatrix& now = matrices_current[rank];
        const ptrdiff_t expected = static_cast<ptrdiff_t>(rank) + rank_shift;
        const ptrdiff_t first = std::max<ptrdiff_t>(0, expected - static_cast<ptrdiff_t>(MATCH_WINDOW));
        const ptrdiff_t last = std::min<ptrdiff_t>(static_cast<ptrdiff_t>(matrices_previous.size()) - 1,
                                                   expected + static_cast<ptrdiff_t>(MATCH_WINDOW));
        ptrdiff_t best = -1;
        f32 best_distance = 0.0f;
        for (ptrdiff_t j = first; j <= last; j++) {
            const LoadedMatrix& before = matrices_previous[j];
            if (!same_object(before, now)) continue;
            const f32 d = distance(before, now);
            if (best < 0 || d < best_distance) {
                best = j;
                best_distance = d;
            }
        }
        return best;
    }

    FrameBuffer* saved_frame() {
        FrameBuffer* buffer = frameBufferList().findBuffer(saved_address);
        return buffer == saved_buffer ? buffer : nullptr;
    }
}

void RT64EX_OnMatrix(u32 address, u8 param, f32 mtx[4][4]) {
    if (!substituting) {
        LoadedMatrix loaded{address, param, {}};
        memcpy(loaded.m, mtx, sizeof(loaded.m));
        matrices_current.push_back(loaded);
        return;
    }
    const size_t rank = substitute_rank++;
    if (rank >= matrices_current.size()) return;
    const ptrdiff_t twin = find_twin(rank);
    if (twin < 0) {
        std::lock_guard lock{match_mutex};
        match_stats.unmatched++;
        return;
    }
    rank_shift = twin - static_cast<ptrdiff_t>(rank);
    const LoadedMatrix& before = matrices_previous[twin];
    const LoadedMatrix& now = matrices_current[rank];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) mtx[r][c] = 0.5f * (before.m[r][c] + now.m[r][c]);
    }
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
    camera_cut = false;
}

// After the first pass. False when this frame gets no in-between image.
extern "C" bool dk64nx_interp_begin_second_pass() {
    FrameBuffer* buffer = frameBufferList().getCurrent();
    if (camera_cut) {
        std::lock_guard lock{match_mutex};
        match_stats.camera_cuts++;
        return false;
    }
    if (matrices_previous.empty() || buffer == nullptr || config.video.multisampling != 0) return false;
    buffer->interpCopy(0, true);
    saved_buffer = buffer;
    saved_address = buffer->m_startAddress;
    substituting = true;
    substitute_rank = 0;
    rank_shift = 0;
    return true;
}

extern "C" void dk64nx_interp_end_second_pass() {
    substituting = false;
    if (FrameBuffer* buffer = saved_frame()) {
        buffer->interpCopy(1, true);   // the in-between frame
        buffer->interpCopy(0, false);  // the real one back
        in_between_pending = true;
        std::lock_guard lock{match_mutex};
        match_stats.interpolated++;
    }
}

// VI: when the frame about to be shown has an in-between image, put it in the color buffer and return true; the
// caller presents it, then calls dk64nx_interp_present_done to put the real frame back for the next VI.
extern "C" bool dk64nx_interp_present_in_between(u32 origin) {
    if (!in_between_pending) return false;
    FrameBuffer* buffer = saved_frame();
    if (buffer == nullptr) {
        in_between_pending = false;
        return false;
    }
    // Until the VI shows that frame's buffer: the swap may land a VI later than the display list.
    if (frameBufferList().findBuffer(origin) != buffer) return false;
    in_between_pending = false;
    buffer->interpCopy(1, false);
    return true;
}

extern "C" void dk64nx_interp_present_done() {
    if (FrameBuffer* buffer = saved_frame()) buffer->interpCopy(0, false);
}

extern "C" int dk64nx_interp_stats(char* out, int size) {
    std::lock_guard lock{match_mutex};
    const MatchStats s = match_stats;
    match_stats = {};
    const double pairs = static_cast<double>(s.equal + s.close + s.far);
    return snprintf(out, size, "frames=%llu interpolated=%llu cuts=%llu unmatched/frame=%.1f matrices/frame=%.0f equal=%.1f%% close=%.1f%% far=%.1f%% count_changed=%llu",
                    (unsigned long long)s.frames, (unsigned long long)s.interpolated, (unsigned long long)s.camera_cuts,
                    s.interpolated ? double(s.unmatched) / s.interpolated : 0.0,
                    s.frames ? double(s.matrices) / s.frames : 0.0,
                    pairs ? 100.0 * s.equal / pairs : 0.0, pairs ? 100.0 * s.close / pairs : 0.0,
                    pairs ? 100.0 * s.far / pairs : 0.0, (unsigned long long)s.count_changed);
}
