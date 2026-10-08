// Player 1's pad in Switch terms, shared by the N64 mapping (input_switch.cpp), the NX Options menu
// (nx_menu.cpp) and the developer test harness (remote_switch.cpp).
#pragma once

#include <cstdint>

enum NxButton : uint32_t {
    NX_A = 1u << 0,
    NX_B = 1u << 1,
    NX_X = 1u << 2,
    NX_Y = 1u << 3,
    NX_L = 1u << 4,
    NX_R = 1u << 5,
    NX_ZL = 1u << 6,
    NX_ZR = 1u << 7,
    NX_PLUS = 1u << 8,
    NX_MINUS = 1u << 9,
    NX_UP = 1u << 10,
    NX_DOWN = 1u << 11,
    NX_LEFT = 1u << 12,
    NX_RIGHT = 1u << 13,
    NX_LSTICK = 1u << 14,
    NX_RSTICK = 1u << 15,
};

struct NxPad {
    uint32_t buttons = 0;
    // Sticks in [-1, 1], dead zone applied, y pointing up.
    float lx = 0.0f;
    float ly = 0.0f;
    float rx = 0.0f;
    float ry = 0.0f;
};

// False when no pad is connected (the harness counts as one in DK64NX_REMOTE builds).
bool dk64nx_read_pad(NxPad* pad);
// True while the NX Options menu owns the pad: the game then sees a neutral pad.
bool dk64nx_menu_owns_pad();
