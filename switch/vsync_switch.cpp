// VI pacing from the display. At 60 fps (interpolated) a VI follows the vblank of the TV (or of the console's screen)
// instead of ultramodern's 60 Hz timer, which drifts against it: images then reach the screen once per refresh,
// without the occasional doubled or skipped one. At 30 fps the timer keeps the pacing the port shipped with.
#include <atomic>
#include <cstdio>

#include <switch.h>

#include "nx_settings.h"

extern "C" Event* dk64nx_vsync_event(Result* rc_out);  // window_switch.c

// What paces the VIs, for the NX Options performance overlay.
std::atomic<bool> dk64nx_vsync_paced{false};

// Ticks of the last vblanks (dev harness frames command: what the display shows, from the presents).
constexpr int VBLANK_RING = 512;
uint64_t dk64nx_vblank_ring[VBLANK_RING];
std::atomic<uint32_t> dk64nx_vblank_count{0};

namespace {
    constexpr u64 VSYNC_TIMEOUT_NS = 50'000'000;  // a missed vblank must not stall the game

    bool logged = false;
}

// VI thread. False when the timer should pace this VI: 30 fps, or no vsync event from the display.
extern "C" bool dk64nx_wait_vsync() {
    if (!nx::interpolating()) {
        dk64nx_vsync_paced = false;
        return false;
    }
    Result rc = 0;
    Event* vsync = dk64nx_vsync_event(&rc);
    if (!logged) {
        logged = true;
        std::printf("[DK64NX] VI pacing at 60 fps: %s (rc 0x%x)\n", vsync != nullptr ? "display vsync" : "timer", rc);
    }
    const bool paced = vsync != nullptr && R_SUCCEEDED(eventWait(vsync, VSYNC_TIMEOUT_NS));
    if (paced) {
        const uint32_t index = dk64nx_vblank_count.load();
        dk64nx_vblank_ring[index % VBLANK_RING] = armGetSystemTick();
        dk64nx_vblank_count = index + 1;
    }
    dk64nx_vsync_paced = paced;
    return paced;
}
