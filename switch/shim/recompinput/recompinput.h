// Switch build: recompinput pulls the RmlUi controls pages, so switch/input_switch.cpp
// reimplements the calls the game code makes, with the same signatures.
#pragma once

#include <cstdint>

namespace recompinput {
    void poll_inputs();
    void set_rumble(int controller_num, bool on);
    void update_rumble();
    void get_gyro_deltas(int controller_num, float* x, float* y);
    void get_mouse_deltas(float* x, float* y);
    void get_right_analog(int controller_num, float* x, float* y);
    void set_right_analog_suppressed(bool suppressed);
    bool game_input_disabled();
}
