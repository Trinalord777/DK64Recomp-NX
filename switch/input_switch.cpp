// SDL GameController -> N64 pad for player 1. Layout follows PaperBoat-NX (NSO N64 convention):
// A->A, B->B, ZL->Z, L->L, R/ZR->R, +->Start, right stick->C buttons, D-pad->D-pad.
#include <atomic>
#include <cmath>
#include <cstdint>

#include <SDL.h>

#include "nx_pad.h"
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

    std::atomic<SDL_GameController*> current_controller{nullptr};
    std::atomic_bool rumble_on{false};
    std::atomic_bool right_analog_suppressed{false};

    float read_axis(SDL_GameController* controller, SDL_GameControllerAxis axis) {
        float value = SDL_GameControllerGetAxis(controller, axis) / AXIS_MAX;
        return std::fabs(value) < STICK_DEADZONE ? 0.0f : value;
    }

    NxPad read_controller(SDL_GameController* controller) {
        struct Binding { SDL_GameControllerButton button; uint32_t nx; };
        static constexpr Binding bindings[] = {
            {SDL_CONTROLLER_BUTTON_A, NX_A},
            {SDL_CONTROLLER_BUTTON_B, NX_B},
            {SDL_CONTROLLER_BUTTON_X, NX_X},
            {SDL_CONTROLLER_BUTTON_Y, NX_Y},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, NX_L},
            {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, NX_R},
            {SDL_CONTROLLER_BUTTON_START, NX_PLUS},
            {SDL_CONTROLLER_BUTTON_BACK, NX_MINUS},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, NX_UP},
            {SDL_CONTROLLER_BUTTON_DPAD_DOWN, NX_DOWN},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, NX_LEFT},
            {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, NX_RIGHT},
            {SDL_CONTROLLER_BUTTON_LEFTSTICK, NX_LSTICK},
            {SDL_CONTROLLER_BUTTON_RIGHTSTICK, NX_RSTICK},
        };
        NxPad pad;
        for (const Binding& binding : bindings) {
            if (SDL_GameControllerGetButton(controller, binding.button)) {
                pad.buttons |= binding.nx;
            }
        }
        // ZL/ZR are analog axes in SDL.
        if (read_axis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.5f) pad.buttons |= NX_ZL;
        if (read_axis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f) pad.buttons |= NX_ZR;
        // SDL's Y axes point down.
        pad.lx = read_axis(controller, SDL_CONTROLLER_AXIS_LEFTX);
        pad.ly = -read_axis(controller, SDL_CONTROLLER_AXIS_LEFTY);
        pad.rx = read_axis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
        pad.ry = -read_axis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
        return pad;
    }

    uint16_t to_n64_buttons(const NxPad& pad) {
        struct Binding { uint32_t nx; uint16_t n64; };
        static constexpr Binding bindings[] = {
            {NX_A, N64_A}, {NX_B, N64_B}, {NX_ZL, N64_Z}, {NX_L, N64_L}, {NX_R, N64_R}, {NX_ZR, N64_R},
            {NX_PLUS, N64_START}, {NX_UP, N64_DPAD_UP}, {NX_DOWN, N64_DPAD_DOWN}, {NX_LEFT, N64_DPAD_LEFT},
            {NX_RIGHT, N64_DPAD_RIGHT},
        };
        uint16_t buttons = 0;
        for (const Binding& binding : bindings) {
            if (pad.buttons & binding.nx) {
                buttons |= binding.n64;
            }
        }
        if (pad.rx < -C_BUTTON_THRESHOLD) buttons |= N64_C_LEFT;
        if (pad.rx > C_BUTTON_THRESHOLD) buttons |= N64_C_RIGHT;
        if (pad.ry > C_BUTTON_THRESHOLD) buttons |= N64_C_UP;
        if (pad.ry < -C_BUTTON_THRESHOLD) buttons |= N64_C_DOWN;
        return buttons;
    }

    // The pad as the game sees it: neutral while the NX Options menu owns it.
    bool read_game_pad(NxPad* pad) {
        const bool connected = dk64nx_read_pad(pad);
        if (dk64nx_menu_owns_pad()) {
            *pad = NxPad{};
        }
        return connected;
    }
}

#ifdef DK64NX_REMOTE
bool dk64nx_remote_apply(NxPad* pad);
#endif

bool dk64nx_read_pad(NxPad* pad) {
    *pad = NxPad{};
    SDL_GameController* controller = current_controller.load();
    bool connected = controller != nullptr;
    if (connected) {
        *pad = read_controller(controller);
    }
#ifdef DK64NX_REMOTE
    connected = dk64nx_remote_apply(pad) || connected;
#endif
    return connected;
}

// Called on the main thread, which owns the SDL event queue.
void dk64nx_input_handle_event(const SDL_Event& event) {
    if (event.type == SDL_CONTROLLERDEVICEADDED && current_controller.load() == nullptr) {
        current_controller.store(SDL_GameControllerOpen(event.cdevice.which));
    }
    else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        SDL_GameController* controller = current_controller.load();
        if (controller != nullptr &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == event.cdevice.which) {
            // ponytail: the handle is dropped, not closed, because the game thread may be reading it.
            current_controller.store(nullptr);
        }
    }
}

bool dk64nx_get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) {
        return false;
    }
    NxPad pad;
    const bool connected = read_game_pad(&pad);
    *buttons = to_n64_buttons(pad);
    *x = pad.lx;
    *y = pad.ly;
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
    SDL_GameController* controller = current_controller.load();
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
    NxPad pad;
    if (controller_num != 0 || right_analog_suppressed.load() || !read_game_pad(&pad)) {
        *x = 0.0f;
        *y = 0.0f;
        return;
    }
    *x = pad.rx;
    *y = pad.ry;
}

void recompinput::set_right_analog_suppressed(bool suppressed) {
    right_analog_suppressed.store(suppressed);
}

bool recompinput::game_input_disabled() {
    return false;
}
