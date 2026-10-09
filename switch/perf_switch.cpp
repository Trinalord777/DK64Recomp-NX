// Clock profiles of NX Options > Performance, applied on the main thread (dk64nx_perf_update, called by the SDL
// event pump): only while the game has focus, re-evaluated when the console is docked or undocked, released when
// the game loses focus or exits. Both profiles raise the clocks in handheld only; docked already runs at the
// docked clocks. Two sources, as in mpo_nx and sa2-nx:
//  - the official apm configuration "docked GPU in handheld" (GPU 768 MHz), with mpo_nx's fallback;
//  - Horizon OC (hoc:clk): the clocks configured in its docked column (this program's profile, else the global
//    one), forced with its override command. No frequency is invented: a module with no docked value is left
//    alone. Overrides do not expire, so a marker file records them and the next start releases them if the game
//    crashed in between.
#include <cstdio>
#include <initializer_list>

#include <switch.h>

#include "librecomp/helpers.hpp"
#include "nx_settings.h"

namespace {
    enum class Profile { ConsoleDefault, DockedGpu, HorizonOcDocked };

    constexpr u32 APM_DOCKED_GPU = 0x00010001;           // GPU 768 MHz
    constexpr u32 APM_DOCKED_GPU_FALLBACK = 0x92220007;  // GPU 460.8 MHz, RAM 1600 MHz

    // Horizon OC's IPC (Source/hoc-clk/common/include/hocclk/ipc.h, API version 2).
    constexpr const char* HOC_SERVICE = "hoc:clk";
    constexpr u32 HOC_GET_PROFILES = 5;
    constexpr u32 HOC_SET_OVERRIDE = 8;
    constexpr u64 HOC_GLOBAL_PROFILE = 0xA111111111111111ull;
    constexpr int HOC_PROFILE_COUNT = 5;
    constexpr int HOC_MODULE_COUNT = 5;      // CPU, GPU, MEM, governor, display
    constexpr int HOC_DOCKED = 4;
    constexpr u32 HOC_CLOCK_MODULES = 3;     // only CPU, GPU and MEM are touched
    constexpr const char* HOC_MARKER = "sdmc:/switch/dk64recomp/.hoc_override";

    struct HocProfiles { u32 mhz[HOC_PROFILE_COUNT][HOC_MODULE_COUNT]; };
    struct HocOverride { u32 module; u32 hz; };

    bool focused = true;
    Profile applied = Profile::ConsoleDefault;
    bool applied_in_handheld = false;
    bool apm_changed = false;
    u32 apm_original = 0;
    AppletHookCookie hook_cookie;

    // Atmosphère's sm extension 65100 answers without blocking; smGetService on a missing service waits forever.
    bool has_service(const char* name) {
        const SmServiceName service = smEncodeName(name);
        u8 present = 0;
        const Result rc = tipcDispatchInOut(smGetServiceSessionTipc(), 65100, service, present);
        return R_SUCCEEDED(rc) && present != 0;
    }

    bool open_hoc(Service* service) {
        return has_service(HOC_SERVICE) && R_SUCCEEDED(smGetService(service, HOC_SERVICE));
    }

    Result hoc_override(Service* service, u32 module, u32 hz) {
        const HocOverride args{module, hz};
        return serviceDispatchIn(service, HOC_SET_OVERRIDE, args);
    }

    Result hoc_profiles(Service* service, u64 program, HocProfiles* out) {
        return serviceDispatchIn(service, HOC_GET_PROFILES, program,
            .buffer_attrs = {SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out},
            .buffers = {{out, sizeof(*out)}});
    }

    void release_hoc() {
        Service service;
        if (!open_hoc(&service)) return;
        for (u32 module = 0; module < HOC_CLOCK_MODULES; module++) {
            hoc_override(&service, module, 0);
        }
        serviceClose(&service);
        std::remove(HOC_MARKER);
        std::printf("[DK64NX] perf: Horizon OC overrides released\n");
    }

    void apply_hoc_docked() {
        Service service;
        if (!open_hoc(&service)) {
            std::printf("[DK64NX] perf: Horizon OC not running\n");
            return;
        }
        u64 program = 0;
        svcGetInfo(&program, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
        HocProfiles own{};
        HocProfiles global{};
        hoc_profiles(&service, program, &own);
        hoc_profiles(&service, HOC_GLOBAL_PROFILE, &global);
        if (FILE* marker = std::fopen(HOC_MARKER, "w")) std::fclose(marker);
        for (u32 module = 0; module < HOC_CLOCK_MODULES; module++) {
            const u32 own_mhz = own.mhz[HOC_DOCKED][module];
            const u32 mhz = own_mhz != 0 ? own_mhz : global.mhz[HOC_DOCKED][module];
            if (mhz == 0) continue;
            const Result rc = hoc_override(&service, module, mhz * 1000000u);
            std::printf("[DK64NX] perf: Horizon OC module %u -> %u MHz (0x%x)\n", module, mhz, rc);
        }
        serviceClose(&service);
    }

    void apply_apm_docked_gpu() {
        if (R_FAILED(apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &apm_original))) return;
        Result rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, APM_DOCKED_GPU);
        if (R_FAILED(rc)) rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, APM_DOCKED_GPU_FALLBACK);
        apm_changed = R_SUCCEEDED(rc);
        std::printf("[DK64NX] perf: apm docked GPU (0x%x)\n", rc);
    }

    void release() {
        if (apm_changed) {
            apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, apm_original);
            apm_changed = false;
        }
        if (applied == Profile::HorizonOcDocked && applied_in_handheld) {
            release_hoc();
        }
        applied = Profile::ConsoleDefault;
    }

    void apply(Profile profile, bool handheld) {
        if (handheld && profile == Profile::DockedGpu) apply_apm_docked_gpu();
        if (handheld && profile == Profile::HorizonOcDocked) apply_hoc_docked();
        applied = profile;
        applied_in_handheld = handheld;
    }

    // Runs inside the SDL event pump (main thread, like dk64nx_perf_update). The clocks are released right here:
    // once the game is in the background it may be suspended before the pump runs again.
    void on_applet_hook(AppletHookType type, void*) {
        if (type != AppletHookType_OnFocusState) return;
        focused = appletGetFocusState() == AppletFocusState_InFocus;
        if (!focused) release();
    }
}

void dk64nx_perf_start() {
    apmInitialize();
    if (FILE* marker = std::fopen(HOC_MARKER, "r")) {
        std::fclose(marker);
        release_hoc();
    }
    appletHook(&hook_cookie, on_applet_hook, nullptr);
}

void dk64nx_perf_update() {
    const Profile wanted = focused ? static_cast<Profile>(nx::get(nx::Setting::ClockProfile)) : Profile::ConsoleDefault;
    const bool handheld = appletGetOperationMode() == AppletOperationMode_Handheld;
    if (wanted == applied && handheld == applied_in_handheld) return;
    release();
    apply(wanted, handheld);
}

void dk64nx_perf_stop() {
    appletUnhook(&hook_cookie);
    release();
    apmExit();
}

// Game logic pacing, read by the patches every frame (getFrameDelta, updateLag, calculateLagBoost). It stays at the
// original 30 fps, two VIs per frame: DK64's engine does not scale everything by the frame's lag, and frames of one
// VI ran the game too fast on the console. 60 fps comes from interpolated frames instead (gliden64_rt64ex.cpp).
extern "C" void recomp_nx_get_frame_vis(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, static_cast<s32>(2));
}

extern "C" void recomp_nx_next_frame_lag(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, static_cast<s32>(2));
}
