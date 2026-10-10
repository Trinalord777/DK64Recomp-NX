// GLideN64 behind ultramodern's RendererContext. RT64 is driven the same way on PC (a
// mupen64plus GFX_INFO over the recomp's RDRAM, see
// lib/RecompFrontend/recompui/src/renderer/rt64_render_context.cpp), so the memory layout
// GLideN64 expects (32-bit words in host order) is already what the recompiled code writes.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

#include <SDL.h>
#include <switch.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_common.h"
#include "m64p_plugin.h"

#include "librecomp/addresses.hpp"
#include "nx_cheats.h"
#include "nx_ram.h"
#include "nx_settings.h"
#include "ultramodern/renderer_context.hpp"

void dk64nx_seed_gliden64_config();
#ifdef DK64NX_REMOTE
void dk64nx_remote_set_rdram(uint8_t* base);
void dk64nx_remote_time(int kind, uint64_t start_tick);
#define DK64NX_TIME(kind, start) dk64nx_remote_time(kind, start)
extern "C" uint32_t dk64nx_interp_shown_position(uint32_t origin, bool in_between_image);
extern "C" uint32_t dk64nx_interp_current_buffer();
extern uint32_t dk64nx_shown_position;
extern uint32_t dk64nx_shown_origin;
// Bit 31 of the origin: the VI is blanked (osViBlack: hStart 0), so GLideN64 presents black.
#define DK64NX_SHOWN(origin, in_between_image) \
    (dk64nx_shown_position = dk64nx_interp_shown_position(origin, in_between_image), \
     dk64nx_shown_origin = (origin) | (ultramodern::renderer::get_vi_regs()->VI_H_START_REG == 0 ? 0x80000000u : 0))
#define DK64NX_DL_BUFFER(index, in_between_image) \
    dk64nx_dl_buffer_ring[(index) % 512] = dk64nx_interp_current_buffer() | ((in_between_image) ? 0x80000000u : 0)
#else
#define DK64NX_TIME(kind, start)
#define DK64NX_SHOWN(origin, in_between_image)
#define DK64NX_DL_BUFFER(index, in_between_image)
#endif
bool dk64nx_front_buffer_redrawn(uint32_t origin);
extern "C" void dk64nx_interp_frame_begin();
extern "C" void dk64nx_rt64ex_save_state();
extern "C" void dk64nx_rt64ex_restore_state();
extern "C" bool dk64nx_interp_begin_second_pass(uint32_t first_pass_us);
extern "C" void dk64nx_interp_end_second_pass();
extern "C" bool dk64nx_interp_present_in_between(uint32_t origin);
extern "C" void dk64nx_interp_present_done();

// Time of the last display list's passes in microseconds (second: 0 when it had no in-between image), for the
// NX Options performance overlay.
std::atomic<uint32_t> dk64nx_pass1_us{0};
std::atomic<uint32_t> dk64nx_pass2_us{0};
// Ticks at which the last display lists started, one per game frame (dev harness frames command): 33.3 ms apart at
// the game's 30 fps; 50 ms means the game itself lost a frame, which shows as a stutter even at 60 presents a second.
uint64_t dk64nx_dl_ring[512];
uint32_t dk64nx_dl_buffer_ring[512];  // color buffer of each display list, bit 31: it got an in-between image
uint32_t dk64nx_dl_us_ring[512];      // its first pass, in microseconds
std::atomic<uint32_t> dk64nx_dl_count{0};

namespace {
    // OSTask is copied where the RSP boot code leaves it, at the end of DMEM.
    constexpr uint32_t TASK_DMEM_OFFSET = 0xFC0;
    constexpr uint32_t KSEG_MASK = 0x1FFFFFFF;
    // libultra's ROM header: GLideN64 reads the internal name from it for its per-game tweaks.
    constexpr char ROM_NAME[] = "DONKEY KONG 64      ";

    uint8_t dmem[0x1000];
    uint8_t imem[0x1000];
    uint8_t rom_header[0x40];
    unsigned int mi_intr_reg;
    unsigned int sp_status_reg;
    unsigned int dpc_start_reg, dpc_end_reg, dpc_current_reg, dpc_status_reg;
    unsigned int dpc_clock_reg, dpc_bufbusy_reg, dpc_pipebusy_reg, dpc_tmem_reg;
    const unsigned int rdram_size = recomp::mem_size;

    // ultramodern signals SP/DP completion itself once send_dl returns.
    void check_interrupts() {}

    void debug_callback(void*, int level, const char* message) {
        if (level <= M64MSG_WARNING) {
            fprintf(stderr, "[GLideN64] %s\n", message);
        }
    }

    // GLideN64 reads the header as 32-bit host-order words, like the ROM image mupen64plus keeps.
    void build_rom_header() {
        memset(rom_header, 0, sizeof(rom_header));
        for (size_t i = 0; i < sizeof(ROM_NAME) - 1; i++) {
            rom_header[(0x20 + i) ^ 3] = static_cast<uint8_t>(ROM_NAME[i]);
        }
    }

    uint32_t to_physical(uint32_t address) {
        return address & KSEG_MASK;
    }

    class GLideN64RendererContext final : public ultramodern::renderer::RendererContext {
    public:
        explicit GLideN64RendererContext(uint8_t* rdram) {
            chosen_api = ultramodern::renderer::GraphicsApi::Auto;
            dk64nx_seed_gliden64_config();
            nx::cheats::set_rdram(rdram);
            nx::ram::base = rdram;
#ifdef DK64NX_REMOTE
            dk64nx_remote_set_rdram(rdram);
#endif
            build_rom_header();

            if (PluginStartup(nullptr, nullptr, debug_callback) != M64ERR_SUCCESS) {
                setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
                return;
            }

            auto* vi = ultramodern::renderer::get_vi_regs();
            GFX_INFO info{};
            info.HEADER = rom_header;
            info.RDRAM = rdram;
            info.DMEM = dmem;
            info.IMEM = imem;
            info.MI_INTR_REG = &mi_intr_reg;
            info.DPC_START_REG = &dpc_start_reg;
            info.DPC_END_REG = &dpc_end_reg;
            info.DPC_CURRENT_REG = &dpc_current_reg;
            info.DPC_STATUS_REG = &dpc_status_reg;
            info.DPC_CLOCK_REG = &dpc_clock_reg;
            info.DPC_BUFBUSY_REG = &dpc_bufbusy_reg;
            info.DPC_PIPEBUSY_REG = &dpc_pipebusy_reg;
            info.DPC_TMEM_REG = &dpc_tmem_reg;
            info.VI_STATUS_REG = &vi->VI_STATUS_REG;
            info.VI_ORIGIN_REG = &vi->VI_ORIGIN_REG;
            info.VI_WIDTH_REG = &vi->VI_WIDTH_REG;
            info.VI_INTR_REG = &vi->VI_INTR_REG;
            info.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
            info.VI_TIMING_REG = &vi->VI_TIMING_REG;
            info.VI_V_SYNC_REG = &vi->VI_V_SYNC_REG;
            info.VI_H_SYNC_REG = &vi->VI_H_SYNC_REG;
            info.VI_LEAP_REG = &vi->VI_LEAP_REG;
            info.VI_H_START_REG = &vi->VI_H_START_REG;
            info.VI_V_START_REG = &vi->VI_V_START_REG;
            info.VI_V_BURST_REG = &vi->VI_V_BURST_REG;
            info.VI_X_SCALE_REG = &vi->VI_X_SCALE_REG;
            info.VI_Y_SCALE_REG = &vi->VI_Y_SCALE_REG;
            info.CheckInterrupts = check_interrupts;
            info.version = 2;
            info.SP_STATUS_REG = &sp_status_reg;
            info.RDRAM_SIZE = &rdram_size;

            if (!InitiateGFX(info) || !RomOpen()) {
                setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
                return;
            }
            setup_result = ultramodern::renderer::SetupResult::Success;
            opened = true;
        }

        ~GLideN64RendererContext() override {
            shutdown();
        }

        bool valid() override { return opened; }

        bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override {
            return true;
        }

        void enable_instant_present() override {}

        void send_dl(const OSTask* task) override {
            // Lay the task out in DMEM as osSpTaskLoad does, with physical addresses.
            const OSTask_s& t = task->t;
            const uint32_t words[16] = {
                t.type, t.flags,
                to_physical(t.ucode_boot), t.ucode_boot_size,
                to_physical(t.ucode), t.ucode_size,
                to_physical(t.ucode_data), t.ucode_data_size,
                to_physical(t.dram_stack), t.dram_stack_size,
                to_physical(t.output_buff), t.output_buff_size,
                to_physical(t.data_ptr), t.data_size,
                to_physical(t.yield_data_ptr), t.yield_data_size,
            };
            memcpy(&dmem[TASK_DMEM_OFFSET], words, sizeof(words));
            // HALT | BROKE | TASKDONE, set before ProcessDList as GFX_INFO v2 requires.
            sp_status_reg = 0x0203;
            const uint64_t start = armGetSystemTick();
            const uint32_t dl_index = dk64nx_dl_count.load();
            dk64nx_dl_ring[dl_index % 512] = start;
            dk64nx_dl_count = dl_index + 1;
            dk64nx_interp_frame_begin();
            dk64nx_rt64ex_save_state();
            ProcessDList();
            const uint64_t second = armGetSystemTick();
            DK64NX_TIME(2, start);
            dk64nx_pass1_us = static_cast<uint32_t>(armTicksToNs(second - start) / 1000);
            dk64nx_pass2_us = 0;
            // 60 fps: the same display list again, with matrices halfway from the previous frame (gliden64_rt64ex.cpp).
            const bool second_pass = nx::interpolated_60() && dk64nx_interp_begin_second_pass(dk64nx_pass1_us);
            DK64NX_DL_BUFFER(dl_index, second_pass);
            dk64nx_dl_us_ring[dl_index % 512] = dk64nx_pass1_us;
            if (second_pass) {
                dk64nx_rt64ex_restore_state();
                sp_status_reg = 0x0203;
                ProcessDList();
                dk64nx_interp_end_second_pass();
                DK64NX_TIME(3, second);
                dk64nx_pass2_us = static_cast<uint32_t>(armTicksToNs(armGetSystemTick() - second) / 1000);
            }
            DK64NX_TIME(0, start);
        }

        void send_dummy_workload(uint32_t) override {}

        void update_screen() override {
            // DK64 starts frame N+2 in the buffer still on screen and relies on the RDP freeze (not
            // emulated) to hold it back, so re-presenting that buffer would show N+2 before N+1:
            // back-and-forth jitter in motion. Keep the image on screen instead; the VI moves to the
            // other buffer next. Bounded so a single-buffered scene still refreshes.
            const uint32_t origin = ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG;
            // 60 fps: a new frame shows its in-between image first; the next VI presents the real one.
            if (dk64nx_interp_present_in_between(origin)) {
                DK64NX_SHOWN(origin, true);
                UpdateScreen();
                dk64nx_interp_present_done();
                held_presents = 0;
                presented_origin = origin;
                return;
            }
            if (origin == presented_origin && held_presents < MAX_HELD_PRESENTS && dk64nx_front_buffer_redrawn(origin)) {
                held_presents++;
            } else {
                held_presents = 0;
                presented_origin = origin;
                [[maybe_unused]] const uint64_t start = armGetSystemTick();
                DK64NX_SHOWN(origin, false);
                UpdateScreen();
                DK64NX_TIME(1, start);
            }
        }

        void shutdown() override {
            if (opened) {
                RomClosed();
                PluginShutdown();
                opened = false;
            }
        }

        uint32_t get_display_framerate() const override { return 60; }
        float get_resolution_scale() const override { return static_cast<float>(nx::resolution_factor()); }

    private:

        bool opened = false;
        // ponytail: 3 held VIs max, so single-buffered scenes drop to 15 Hz at worst; a real RDP freeze would avoid it.
        static constexpr uint32_t MAX_HELD_PRESENTS = 3;
        uint32_t presented_origin = 0;
        uint32_t held_presents = 0;
    };
}

std::unique_ptr<ultramodern::renderer::RendererContext> dk64nx_create_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
    return std::make_unique<GLideN64RendererContext>(rdram);
}
