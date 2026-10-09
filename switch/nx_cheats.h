// GameShark cheats of the NX Options menu, read from sdmc:/switch/dk64recomp/cheats.cht (libretro's format, e.g.
// libretro-database's "Donkey Kong 64 (U).cht"). Only codes that write data work: the game runs as recompiled
// code, so a code that patches MIPS instructions changes nothing.
#pragma once

#include <cstdint>

namespace nx::cheats {
    // Startup: reads the cheats and which ones were left on (cheats_enabled.txt).
    void load();
    void set_rdram(uint8_t* rdram);
    // Graphics thread, once per presented frame.
    void apply();

    int count();
    const char* name(int index);
    // False when the code uses a type this engine does not run (only 80/81/A0/A1 writes and D0-D3 conditions).
    bool supported(int index);
    bool enabled(int index);
    void toggle(int index);
    // Writes cheats_enabled.txt if a cheat was toggled since the last save.
    void save();
}
