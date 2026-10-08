// SDL GameController -> N64 pad for player 1. Layout follows PaperBoat-NX (NSO N64 convention):
// A->A, B->B, ZL->Z, L->L, R/ZR->R, +->Start, right stick->C buttons, D-pad->D-pad.
#include <atomic>
#include <cmath>
#include <cstdint>

#include <SDL.h>

#include "recompinput/recompinput.h"
#include "ultramodern/input.hpp"

namespace {
    constexpr uint16_t N64_A = 0x8000;
    constexpr uint16_t N64_B = 0x4000;
    constexpr uint16_t N64_Z = 0x2000;
    constexpr uint16_t N64_START = 0x1000;
    constexpr uint16_t N64_DPAD_UP = 0x0800;
    constexpr uint16_t N64_DPAD_DOWN = 0x0400;
    constexpr uint16_t N64_DPAD_LEFT = 0x0200;
    constexpr uint16_t N64_DPAD_RIGHT = 0x0100;
    constexpr uint16_t N64_L = 0x0020;
    constexpr uint16_t N64_R = 0x0010;
    constexpr uint16_t N64_C_UP = 0x0008;
    constexpr uint16_t N64_C_DOWN = 0x0004;
    constexpr uint16_t N64_C_LEFT = 0x0002;
    constexpr uint16_t N64_C_RIGHT = 0x0001;

    constexpr float AXIS_MAX = 32767.0f;
    // Right-stick deflection that counts as a C-button press.
    constexpr float C_BUTTON_THRESHOLD = 0.5f;
    constexpr float STICK_DEADZONE = 0.1f;
    constexpr uint16_t RUMBLE_STRENGTH = 0x6000;
    // Re-armed on every VI while the game keeps rumble on, so a short duration stops it promptly.
    constexpr uint32_t RUMBLE_PULSE_MS = 100;

    std::atomic<SDL_GameController*> pad{nullptr};
    std::atomic_bool rumble_on{false};
    std::atomic_bool right_analog_suppressed{false};

    float read_axis(SDL_GameController* controller, SDL_GameControllerAxis axis) {
        float value = SDL_GameControllerGetAxis(controller, axis) / AXIS_MAX;
        return std::fabs(value) < STICK_DEADZONE ? 0.0f : value;
    }

    uint16_t read_buttons(SDL_GameController* controller) {
        struct Binding { SDL_GameControllerButton button; uint16_t n64; };
        static constexpr Binding bindings[] = {
            {SDL_CONTROLLER_BUTTON_A, N64_A},
            {SDL_CONTROLLER_BUTTON_B, N64_B},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, N64_L},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, N64_R},
            {SDL_CONTROLLER_BUTTON_START, N64_START},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, N64_DPAD_UP},
            {SDL_CONTROLLER_BUTTON_DPAD_DOWN, N64_DPAD_DOWN},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, N64_DPAD_LEFT},
            {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, N64_DPAD_RIGHT},
        };
        uint16_t buttons = 0;
        for (const Binding& binding : bindings) {
            if (SDL_GameControllerGetButton(controller, binding.button)) {
                buttons |= binding.n64;
            }
        }
        // ZL/ZR are analog axes in SDL.
        if (read_axis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.5f) buttons |= N64_Z;
        if (read_axis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f) buttons |= N64_R;

        float cx = read_axis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
        float cy = read_axis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
        if (cx < -C_BUTTON_THRESHOLD) buttons |= N64_C_LEFT;
        if (cx > C_BUTTON_THRESHOLD) buttons |= N64_C_RIGHT;
        if (cy < -C_BUTTON_THRESHOLD) buttons |= N64_C_UP;
        if (cy > C_BUTTON_THRESHOLD) buttons |= N64_C_DOWN;
        return buttons;
    }
}

// Called on the main thread, which owns the SDL event queue.
void dk64nx_input_handle_event(const SDL_Event& event) {
    if (event.type == SDL_CONTROLLERDEVICEADDED && pad.load() == nullptr) {
        pad.store(SDL_GameControllerOpen(event.cdevice.which));
    }
    else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        SDL_GameController* controller = pad.load();
        if (controller != nullptr &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == event.cdevice.which) {
            // ponytail: the handle is dropped, not closed, because the game thread may be reading it.
            pad.store(nullptr);
        }
    }
}

#ifdef DK64NX_REMOTE
bool dk64nx_remote_apply(uint16_t* buttons, float* x, float* y);
#endif

bool dk64nx_get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) {
        return false;
    }
    SDL_GameController* controller = pad.load();
    bool connected = controller != nullptr;
    *buttons = 0;
    *x = 0.0f;
    *y = 0.0f;
    if (connected) {
        *buttons = read_buttons(controller);
        *x = read_axis(controller, SDL_CONTROLLER_AXIS_LEFTX);
        // SDL's Y axis points down, the N64's points up.
        *y = -read_axis(controller, SDL_CONTROLLER_AXIS_LEFTY);
    }
#ifdef DK64NX_REMOTE
    connected = dk64nx_remote_apply(buttons, x, y) || connected;
#endif
    return connected;
}

ultramodern::input::connected_device_info_t dk64nx_get_connected_device_info(int controller_num) {
    if (controller_num == 0) {
        return {ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak};
    }
    return {ultramodern::input::Device::None, ultramodern::input::Pak::None};
}

// The main thread pumps SDL events every millisecond, so controller state is already fresh.
void recompinput::poll_inputs() {}

void recompinput::set_rumble(int controller_num, bool on) {
    if (controller_num == 0) {
        rumble_on.store(on);
    }
}

void recompinput::update_rumble() {
    SDL_GameController* controller = pad.load();
    if (controller != nullptr && rumble_on.load()) {
        SDL_GameControllerRumble(controller, RUMBLE_STRENGTH, RUMBLE_STRENGTH, RUMBLE_PULSE_MS);
    }
}

void recompinput::get_gyro_deltas(int, float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
}

void recompinput::get_mouse_deltas(float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
}

void recompinput::get_right_analog(int controller_num, float* x, float* y) {
    SDL_GameController* controller = pad.load();
    if (controller_num != 0 || controller == nullptr || right_analog_suppressed.load()) {
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    *x = read_axis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
    *y = -read_axis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
}

void recompinput::set_right_analog_suppressed(bool suppressed) {
    right_analog_suppressed.store(suppressed);
}

bool recompinput::game_input_disabled() {
    return false;
}
