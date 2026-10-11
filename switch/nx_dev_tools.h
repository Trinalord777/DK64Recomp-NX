// NX Options > Dev Tools, as in the HarbourMasters ports' developer menu: readouts of the running game (map, Kong,
// position, interpolation) and actions on it (warp, inventory, flags). Addresses: DK64 decomp, USA ROM
// (lib/dk64_decomp/symbol_addrs.us.txt).
#pragma once

#include <cstdint>
#include <string>

namespace nx::dev {
    struct Tool {
        const char* section;
        const char* label;
        const char* description;
        std::string (*value)();
        void (*change)(int direction, bool wrap);  // nullptr: a readout. wrap: A rather than Left/Right
        bool button = false;  // an action run by A, value() being the button's text
    };

    int count();
    const Tool& tool(int index);
    // While the menu is open: refreshes the per-second readouts.
    void update(uint64_t now_ms);
    // "CPU 1020, GPU 768, RAM 1600 MHz", read from the console now (a few IPC calls).
    std::string read_clocks();
    // Dev Tools > Game > Map & Position Overlay, after the Paper Mario port's Value Viewer: on, and its text.
    bool overlay_shown();
    std::string overlay_text();
    // The current file's play time in seconds, as the file select shows it; -1 outside a single-player adventure.
    long play_time_seconds();
    // Warp To any map and exit, taken by the game thread at its next frame (dev harness warp command).
    bool request_warp(int map, int exit);  // false outside an adventure
}
