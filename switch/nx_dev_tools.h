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
    };

    int count();
    const Tool& tool(int index);
    // While the menu is open: refreshes the per-second readouts.
    void update(uint64_t now_ms);
    // "CPU 1020, GPU 768, RAM 1600 MHz", read from the console now (a few IPC calls).
    std::string read_clocks();
}
