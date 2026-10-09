// VI pacing from the display. At 60 fps (interpolated) a VI follows the vblank of the TV (or of the console's screen)
// instead of ultramodern's 60 Hz timer, which drifts against it: frames then reach the screen once per refresh,
// without the occasional doubled or skipped one. At 30 fps the timer keeps the pacing the port shipped with.
#include <cstdio>

#include <switch.h>

#include "nx_settings.h"

namespace {
    constexpr u64 VSYNC_TIMEOUT_NS = 50'000'000;  // a missed vblank must not stall the game

    enum class State { NotOpened, Open, Unavailable };
    State state = State::NotOpened;
    ViDisplay display;
    Event vsync;
}

// VI thread. False when the timer should pace this VI: 30 fps, or no vsync event from the display.
extern "C" bool dk64nx_wait_vsync() {
    if (!nx::interpolated_60()) return false;
    if (state == State::NotOpened) {
        // libnx already opened the default display for its window, and vi refuses a second open (0x1272): ask for the
        // vsync event of the display ids the session can hold instead.
        Result rc = viOpenDefaultDisplay(&display);
        u64 id = R_SUCCEEDED(rc) ? display.display_id : 0;
        if (R_SUCCEEDED(rc)) {
            rc = viGetDisplayVsyncEvent(&display, &vsync);
        }
        for (u64 candidate = 0; R_FAILED(rc) && candidate < 8; candidate++) {
            display = ViDisplay{};
            display.display_id = candidate;
            display.initialized = true;
            rc = viGetDisplayVsyncEvent(&display, &vsync);
            id = candidate;
        }
        state = R_SUCCEEDED(rc) ? State::Open : State::Unavailable;
        std::printf("[DK64NX] VI pacing at 60 fps: %s (display id %llu, rc 0x%x)\n",
                    state == State::Open ? "display vsync" : "timer", static_cast<unsigned long long>(id), rc);
    }
    return state == State::Open && R_SUCCEEDED(eventWait(&vsync, VSYNC_TIMEOUT_NS));
}
