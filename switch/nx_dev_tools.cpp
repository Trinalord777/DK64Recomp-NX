#include "nx_dev_tools.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

#include <switch.h>

#include "dk64_maps.h"
#include "librecomp/helpers.hpp"
#include "nx_ram.h"
#include "nx_settings.h"

extern "C" void dk64nx_interp_totals(uint64_t out[4]);  // gliden64_rt64ex.cpp

namespace {
    // Game state, by decomp name.
    constexpr uint32_t CURRENT_MAP = 0x8076A0A8;          // current_map, Maps (s32)
    constexpr uint32_t CURRENT_CHARACTER = 0x8074E77C;    // current_character_index[0], u8 (kongs_e)
    constexpr uint32_t PLAYER_POINTER = 0x807FBB4C;       // gPlayerPointer, Actor*
    constexpr uint32_t ACTOR_POSITION = 0x7C;             // Actor x_position, y_position, z_position: f32
    constexpr uint32_t PLAYER_PROGRESS = 0x807FC950;      // D_global_asm_807FC950[0], PlayerProgress
    constexpr uint32_t CHARACTER_PROGRESS_SIZE = 0x5E;    // PlayerProgress.character_progress[6], one per Kong
    constexpr uint32_t COINS = 0x06;                      // CharacterProgress.coins, u16
    constexpr uint32_t STANDARD_AMMO = PLAYER_PROGRESS + 0x2F0;  // u16
    constexpr uint32_t ORANGES = PLAYER_PROGRESS + 0x2F4;        // u16
    constexpr uint32_t CRYSTALS = PLAYER_PROGRESS + 0x2F6;       // u16, 150 per crystal coconut on screen
    constexpr uint32_t FILM = PLAYER_PROGRESS + 0x2F8;           // u16
    constexpr uint32_t HEALTH = PLAYER_PROGRESS + 0x2FB;         // s8, quarters of a melon
    constexpr uint32_t MELONS = PLAYER_PROGRESS + 0x2FC;         // u8
    constexpr int CRYSTAL_UNITS = 150;
    // Permanent flags: the current file's block of the save data (getFlagBlockAddress, saveFile.c).
    constexpr uint32_t SAVE_DATA = 0x807ECEA8;            // D_global_asm_807ECEA8
    constexpr uint32_t FLAG_BITS = 0x807ECEA0;            // D_global_asm_807ECEA0, s16: sizes a file
    constexpr uint32_t FILE_SLOTS = 0x807EDEA8;           // D_global_asm_807EDEA8[4]: EEPROM slot of each file
    constexpr uint32_t CURRENT_FILE = 0x807467C8;         // current_file, u8
    constexpr int PERMANENT_FLAG_COUNT = 0x316;           // permflag_e
    constexpr int FLAG_DIDDY = 0x06, FLAG_TINY = 0x42, FLAG_LANKY = 0x46, FLAG_CHUNKY = 0x75;  // PERMFLAG_KONG_*

    constexpr const char* KONG_NAMES[] = {"DK", "Diddy", "Lanky", "Tiny", "Chunky", "Krusha", "Rambi", "Enguarde"};

    // Destinations of DK64 Recompiled's debug warp menu (patches/warp_menu.c), plus DK Isles.
    struct Warp { const char* name; int map; int exit; };
    constexpr Warp WARPS[] = {
        {"DK Isles", 0x22, 0x00},        {"Jungle Japes", 0x07, 0x0F}, {"Angry Aztec", 0x26, 0x00},
        {"Frantic Factory", 0x1A, 0x00}, {"Gloomy Galleon", 0x1E, 0x00}, {"Fungi Forest", 0x30, 0x1B},
        {"Crystal Caves", 0x48, 0x00},   {"Creepy Castle", 0x57, 0x00},   {"Hideout Helm", 0x11, 0x00},
    };
    constexpr int WARP_COUNT = static_cast<int>(sizeof(WARPS) / sizeof(WARPS[0]));
    int warp_choice = 0;
    // map << 8 | exit, taken once per frame by the game thread (recomp_nx_take_warp).
    std::atomic<int32_t> pending_warp{-1};
    int flag_choice = 0;

    std::string text(const char* pattern, double value) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), pattern, value);
        return buffer;
    }

    // Numbers change by step with Left/Right; A adds a step and wraps to the minimum past the maximum.
    int stepped(int value, int direction, bool wrap, int step, int min, int max) {
        const int next = value + direction * step;
        if (wrap && next > max) return min;
        return std::clamp(next, min, max);
    }

    int current_kong() { return nx::ram::u8(CURRENT_CHARACTER); }
    uint32_t kong_coins() { return PLAYER_PROGRESS + std::min(current_kong(), 5) * CHARACTER_PROGRESS_SIZE + COINS; }

    uint32_t permanent_flags() {
        const int bits = static_cast<int16_t>(nx::ram::u16(FLAG_BITS));
        const uint32_t file_bytes = static_cast<uint32_t>((((bits + 0x6B7) & -64) + 0x27) & -8) / 8;
        const uint8_t file = nx::ram::u8(CURRENT_FILE);
        uint32_t slot = 0;
        for (uint32_t i = 0; i < 4; i++) {
            if (nx::ram::u8(FILE_SLOTS + i) == file) {
                slot = i;
                break;
            }
        }
        return SAVE_DATA + slot * file_bytes;
    }

    bool flag(int index) { return (nx::ram::u8(permanent_flags() + index / 8) >> (index % 8)) & 1; }
    void toggle_flag(int index) {
        const uint32_t address = permanent_flags() + index / 8;
        nx::ram::set_u8(address, nx::ram::u8(address) ^ (1u << (index % 8)));
    }

    // CPU, GPU and RAM clocks as the console runs them (clkrst), read once a second while the menu is open.
    std::string clocks_text = "-";

    // Interpolation over the last second.
    uint64_t interp_window[4] = {};
    uint64_t interp_window_ms = 0;
    double interp_share = 0.0, interp_cuts = 0.0, interp_unmatched = 0.0;

    const nx::dev::Tool TOOLS[] = {
        {"Game", "Map", "The map the game is in: its ID and its name in the DK64 decomp.",
         [] {
             const uint32_t map = nx::ram::u32(CURRENT_MAP);
             char buffer[64];
             std::snprintf(buffer, sizeof(buffer), "0x%02X  %s", map, map < static_cast<uint32_t>(DK64_MAP_COUNT) ? DK64_MAP_NAMES[map] : "?");
             return std::string(buffer);
         }, nullptr},
        {"Game", "Kong", "The character the player controls.",
         [] {
             const int kong = current_kong();
             return std::string(kong < 8 ? KONG_NAMES[kong] : "?");
         }, nullptr},
        {"Game", "Position", "The player's x, y and z in the map's units.",
         [] {
             const uint32_t actor = nx::ram::u32(PLAYER_POINTER);
             if (!nx::ram::valid(actor)) return std::string("-");
             char buffer[64];
             std::snprintf(buffer, sizeof(buffer), "%.0f, %.0f, %.0f", nx::ram::f32(actor + ACTOR_POSITION),
                           nx::ram::f32(actor + ACTOR_POSITION + 4), nx::ram::f32(actor + ACTOR_POSITION + 8));
             return std::string(buffer);
         }, nullptr},

        {"Game", "Clocks",
         "CPU, GPU and RAM clocks the console runs at right now, overclock included (refreshed every second).",
         [] { return clocks_text; }, nullptr},

        {"Warp", "Warp To",
         "Left/Right: a level. A: go there now, as DK64 Recompiled's debug warp menu does. The game ignores it "
         "during some cutscenes and menus.",
         [] { return std::string(pending_warp.load() >= 0 ? "Warping..." : WARPS[warp_choice].name); },
         [](int direction, bool wrap) {
             if (wrap) {
                 pending_warp = WARPS[warp_choice].map << 8 | WARPS[warp_choice].exit;
                 return;
             }
             warp_choice = (warp_choice + direction + WARP_COUNT) % WARP_COUNT;
         }},

        {"Inventory", "Coins (Current Kong)", "The current Kong's coins.",
         [] { return std::to_string(nx::ram::u16(kong_coins())); },
         [](int direction, bool wrap) {
             nx::ram::set_u16(kong_coins(), static_cast<uint16_t>(stepped(nx::ram::u16(kong_coins()), direction, wrap, 1, 0, 999)));
         }},
        {"Inventory", "Standard Ammo", "Ammunition of the Kongs' guns.",
         [] { return std::to_string(nx::ram::u16(STANDARD_AMMO)); },
         [](int direction, bool wrap) {
             nx::ram::set_u16(STANDARD_AMMO, static_cast<uint16_t>(stepped(nx::ram::u16(STANDARD_AMMO), direction, wrap, 10, 0, 200)));
         }},
        {"Inventory", "Oranges", "Orange grenades.",
         [] { return std::to_string(nx::ram::u16(ORANGES)); },
         [](int direction, bool wrap) {
             nx::ram::set_u16(ORANGES, static_cast<uint16_t>(stepped(nx::ram::u16(ORANGES), direction, wrap, 1, 0, 20)));
         }},
        {"Inventory", "Crystal Coconuts", "Crystals that power the Kongs' special moves.",
         [] { return std::to_string(nx::ram::u16(CRYSTALS) / CRYSTAL_UNITS); },
         [](int direction, bool wrap) {
             const int crystals = stepped(nx::ram::u16(CRYSTALS) / CRYSTAL_UNITS, direction, wrap, 1, 0, 20);
             nx::ram::set_u16(CRYSTALS, static_cast<uint16_t>(crystals * CRYSTAL_UNITS));
         }},
        {"Inventory", "Film", "Film for the Banana Fairy camera.",
         [] { return std::to_string(nx::ram::u16(FILM)); },
         [](int direction, bool wrap) {
             nx::ram::set_u16(FILM, static_cast<uint16_t>(stepped(nx::ram::u16(FILM), direction, wrap, 1, 0, 20)));
         }},
        {"Inventory", "Melons", "Health containers.",
         [] { return std::to_string(nx::ram::u8(MELONS)); },
         [](int direction, bool wrap) { nx::ram::set_u8(MELONS, static_cast<uint8_t>(stepped(nx::ram::u8(MELONS), direction, wrap, 1, 1, 3))); }},
        {"Inventory", "Health", "Health in quarters of a melon (4 per melon).",
         [] { return std::to_string(static_cast<int8_t>(nx::ram::u8(HEALTH))); },
         [](int direction, bool wrap) {
             const int health = stepped(static_cast<int8_t>(nx::ram::u8(HEALTH)), direction, wrap, 1, 0, nx::ram::u8(MELONS) * 4);
             nx::ram::set_u8(HEALTH, static_cast<uint8_t>(health));
         }},

        {"Flags", "Diddy Unlocked", "Permanent flag 0x06 of the current file. Save from the pause menu to keep it.",
         [] { return std::string(flag(FLAG_DIDDY) ? "On" : "Off"); }, [](int, bool) { toggle_flag(FLAG_DIDDY); }},
        {"Flags", "Lanky Unlocked", "Permanent flag 0x46 of the current file. Save from the pause menu to keep it.",
         [] { return std::string(flag(FLAG_LANKY) ? "On" : "Off"); }, [](int, bool) { toggle_flag(FLAG_LANKY); }},
        {"Flags", "Tiny Unlocked", "Permanent flag 0x42 of the current file. Save from the pause menu to keep it.",
         [] { return std::string(flag(FLAG_TINY) ? "On" : "Off"); }, [](int, bool) { toggle_flag(FLAG_TINY); }},
        {"Flags", "Chunky Unlocked", "Permanent flag 0x75 of the current file. Save from the pause menu to keep it.",
         [] { return std::string(flag(FLAG_CHUNKY) ? "On" : "Off"); }, [](int, bool) { toggle_flag(FLAG_CHUNKY); }},
        {"Flags", "Permanent Flag",
         "Left/Right: any of the 790 permanent flags (permflag_e in the decomp's enums.h). A: turn it on or off. "
         "Save from the pause menu to keep it.",
         [] {
             char buffer[32];
             std::snprintf(buffer, sizeof(buffer), "0x%03X  %s", flag_choice, flag(flag_choice) ? "On" : "Off");
             return std::string(buffer);
         },
         [](int direction, bool wrap) {
             if (wrap) {
                 toggle_flag(flag_choice);
                 return;
             }
             flag_choice = (flag_choice + direction + PERMANENT_FLAG_COUNT) % PERMANENT_FLAG_COUNT;
         }},

        {"Interpolation", "Frames With an In-Between Image",
         "At 60 fps, the share of the game's frames that got an in-between image over the last second. Camera cuts "
         "and loading screens get none.",
         [] { return nx::interpolated_60() ? text("%.0f%%", interp_share) : std::string("60 fps off"); }, nullptr},
        {"Interpolation", "Camera Cuts", "Frames over the last second where the camera jumped, shown without an in-between image.",
         [] { return text("%.1f /s", interp_cuts); }, nullptr},
        {"Interpolation", "Unmatched Matrices",
         "Matrices per in-between image that had no counterpart in the previous frame (an object appearing), drawn "
         "where they are instead of halfway.",
         [] { return text("%.1f per frame", interp_unmatched); }, nullptr},
    };
}

int nx::dev::count() { return static_cast<int>(sizeof(TOOLS) / sizeof(TOOLS[0])); }
const nx::dev::Tool& nx::dev::tool(int index) { return TOOLS[index]; }

std::string nx::dev::read_clocks() {
    if (R_FAILED(clkrstInitialize())) return "clkrst unavailable";
    const PcvModuleId modules[] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
    u32 mhz[3] = {};
    for (int i = 0; i < 3; i++) {
        ClkrstSession session;
        u32 hz = 0;
        if (R_SUCCEEDED(clkrstOpenSession(&session, modules[i], 3))) {
            clkrstGetClockRate(&session, &hz);
            clkrstCloseSession(&session);
        }
        mhz[i] = hz / 1000000;
    }
    clkrstExit();
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "CPU %u, GPU %u, RAM %u MHz", mhz[0], mhz[1], mhz[2]);
    return buffer;
}

void nx::dev::update(uint64_t now_ms) {
    if (now_ms - interp_window_ms < 1000) return;
    clocks_text = read_clocks();
    uint64_t totals[4];
    dk64nx_interp_totals(totals);
    const double frames = static_cast<double>(totals[0] - interp_window[0]);
    const double interpolated = static_cast<double>(totals[1] - interp_window[1]);
    interp_share = frames > 0 ? 100.0 * interpolated / frames : 0.0;
    interp_cuts = static_cast<double>(totals[2] - interp_window[2]) * 1000.0 / static_cast<double>(now_ms - interp_window_ms);
    interp_unmatched = interpolated > 0 ? static_cast<double>(totals[3] - interp_window[3]) / interpolated : 0.0;
    std::copy(totals, totals + 4, interp_window);
    interp_window_ms = now_ms;
}

// Game thread, once per frame (patches/boot_logos_patches.c): the warp chosen in Dev Tools, or -1.
extern "C" void recomp_nx_take_warp(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, static_cast<s32>(pending_warp.exchange(-1)));
}
