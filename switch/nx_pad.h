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

// + alone is the game's Start, - and + together open NX Options. The game gets + only once it has been held alone
// for DELAY_MS, so the combo never pauses the game; a tap shorter than that is replayed for DELAY_MS on release.
struct NxStartFilter {
    static constexpr uint64_t DELAY_MS = 100;
    uint64_t plus_since = 0;
    uint64_t replay_until = 0;
    bool combo = false;

    uint32_t apply(uint32_t buttons, uint64_t now) {
        if ((buttons & NX_PLUS) == 0) {
            if (plus_since != 0 && !combo && now - plus_since < DELAY_MS) replay_until = now + DELAY_MS;
            plus_since = 0;
            combo = false;
            return now < replay_until ? buttons | NX_PLUS : buttons;
        }
        if (plus_since == 0) plus_since = now;
        if (buttons & NX_MINUS) combo = true;
        if (combo || now - plus_since < DELAY_MS) buttons &= ~NX_PLUS;
        return buttons;
    }
};

// False when no pad is connected (the harness counts as one in DK64NX_REMOTE builds).
bool dk64nx_read_pad(NxPad* pad);
// True while the NX Options menu owns the pad: the game then sees a neutral pad.
bool dk64nx_menu_owns_pad();
