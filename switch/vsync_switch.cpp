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
        const bool open = R_SUCCEEDED(viOpenDefaultDisplay(&display)) && R_SUCCEEDED(viGetDisplayVsyncEvent(&display, &vsync));
        state = open ? State::Open : State::Unavailable;
        std::printf("[DK64NX] VI pacing above 30 fps: %s\n", open ? "display vsync" : "timer (no vsync event)");
    }
    return state == State::Open && R_SUCCEEDED(eventWait(&vsync, VSYNC_TIMEOUT_NS));
}
