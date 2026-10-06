// Switch build: recompui (RmlUi menus drawn through RT64) is not compiled.
// Only the entry points the game code calls are declared; switch/frontend_switch.cpp defines them.
#pragma once

#include <filesystem>
#include <functional>
#include <list>
#include <string>

// The real header reaches librecomp's ROM helpers (do_rom_read, do_rom_pio) transitively.
#include "librecomp/game.hpp"

namespace recompui {
    void message_box(const char* msg);
    void get_window_size(int& width, int& height);
}
