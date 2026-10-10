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
    // CharacterProgress fields (decomp structs.h), u8 unless noted.
    constexpr uint32_t MOVES = 0x00, SIMIAN_SLAM = 0x01, WEAPON = 0x02, AMMO_BELT = 0x03, INSTRUMENT = 0x04;
    constexpr uint32_t INSTRUMENT_ENERGY = 0x08;          // u16
    constexpr int KONG_COUNT = 5;                         // DK, Diddy, Lanky, Tiny, Chunky (Krusha is the 6th)
    constexpr uint32_t GAME_MODE = 0x80755318, GAME_MODE_COPY = 0x80755314;  // game_mode, game_mode_copy: u8
    constexpr uint8_t GAME_MODE_ADVENTURE = 6;            // game_mode_e
    constexpr uint32_t PLAYER_COUNT = 0x807FC928;         // cc_number_of_players, u8
    // The file's play time (decomp code_450.c, func_global_asm_805FC98C): seconds saved when the file loaded plus the
    // OSTime since then, stopped at the pause's start while paused.
    constexpr uint32_t SAVED_PLAY_SECONDS = 0x80750AB0;   // D_global_asm_80750AB0, u32
    constexpr uint32_t SESSION_START = 0x807445B8;        // D_global_asm_807445B8, OSTime (u64)
    constexpr uint32_t PAUSE_START = 0x807445C0;          // D_global_asm_807445C0, OSTime
    constexpr uint32_t GLOBAL_PROPERTIES = 0x807FBB60;    // global_properties_bitfield, bit 1: paused
    constexpr uint64_t OSTIME_PER_SECOND = 46875000;      // osGetTime counts at 46.875 MHz
    constexpr uint32_t HOMING_AMMO = PLAYER_PROGRESS + 0x2F2;    // u16
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
    // permflag_e: PERMFLAG_ITEM_MOVE_SHOCKWAVE_CAMERA, the training barrels' moves, the 20 Banana Fairies.
    constexpr int FLAG_CAMERA = 0x179;
    constexpr int TRAINING_FLAGS[] = {0x182, 0x183, 0x184, 0x185};  // diving, vines, orange and barrel throwing
    constexpr int FLAG_FIRST_FAIRY = 0x24D, FAIRY_COUNT = 20;

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

    void set_flag(int index) {
        const uint32_t address = permanent_flags() + index / 8;
        nx::ram::set_u8(address, nx::ram::u8(address) | (1u << (index % 8)));
    }

    // Capacities as the game computes them (decomp hud.c, getMaxItemCapacity), for the current Kong's upgrades.
    uint32_t kong_base(int kong) { return PLAYER_PROGRESS + kong * CHARACTER_PROGRESS_SIZE; }
    // The current Kong's belt; on Rambi, Enguarde or Krusha (5 and up) the largest of the five Kongs', since which
    // Kong rides is not kept at a fixed address.
    int ammo_belt() {
        if (current_kong() < KONG_COUNT) return nx::ram::u8(kong_base(current_kong()) + AMMO_BELT);
        int belt = 0;
        for (int kong = 0; kong < KONG_COUNT; kong++) belt = std::max<int>(belt, nx::ram::u8(kong_base(kong) + AMMO_BELT));
        return belt;
    }
    int max_ammo() { return (1 << std::min(ammo_belt(), 2)) * 50; }
    int max_oranges() { return ammo_belt() * 5 + 20; }
    int fairies() {
        int count = 0;
        for (int i = 0; i < FAIRY_COUNT; i++) count += flag(FLAG_FIRST_FAIRY + i);
        return count;
    }
    // Crystals and film have a capacity once the camera and shockwave are learned (the game also counts crystals
    // when a Kong learned its first special move; this keeps to the camera, the case it always covers).
    int max_crystal_units() { return flag(FLAG_CAMERA) ? fairies() * CRYSTAL_UNITS + 3000 : 0; }
    int max_film() { return flag(FLAG_CAMERA) ? fairies() + 10 : 0; }
    int max_instrument_energy(int kong) {
        const int instrument = nx::ram::u8(kong_base(kong) + INSTRUMENT);
        if ((instrument & 1) == 0) return 0;
        int levels = 1;
        for (int bits = instrument; bits != 0; bits >>= 1) levels++;
        return levels * 5;
    }

    // The game keeps permanent flags in a scratch block, not the file's, in DK TV and multiplayer
    // (getFlagBlockAddress): the Unlocks act in a single-player adventure only, so they never reach a save there.
    bool in_adventure() {
        return nx::ram::valid(PLAYER_PROGRESS) && nx::ram::u8(PLAYER_COUNT) < 2 &&
               (nx::ram::u8(GAME_MODE) == GAME_MODE_ADVENTURE || nx::ram::u8(GAME_MODE_COPY) == GAME_MODE_ADVENTURE);
    }

    // As the game's own debug code gives them (decomp code_CC800.c, func_global_asm_806C9C80), for every Kong, with
    // the largest ammo belt and the training barrels' moves.
    void give_all_moves() {
        for (int kong = 0; kong < KONG_COUNT; kong++) {
            nx::ram::set_u8(kong_base(kong) + MOVES, 3);
            nx::ram::set_u8(kong_base(kong) + SIMIAN_SLAM, 3);
            nx::ram::set_u8(kong_base(kong) + WEAPON, 7);
            nx::ram::set_u8(kong_base(kong) + AMMO_BELT, 2);
            nx::ram::set_u8(kong_base(kong) + INSTRUMENT, 0x0F);
        }
        set_flag(FLAG_CAMERA);
        for (int index : TRAINING_FLAGS) set_flag(index);
    }

    void refill_consumables() {
        nx::ram::set_u16(STANDARD_AMMO, static_cast<uint16_t>(max_ammo()));
        nx::ram::set_u16(HOMING_AMMO, static_cast<uint16_t>(max_ammo()));
        nx::ram::set_u16(ORANGES, static_cast<uint16_t>(max_oranges()));
        if (max_crystal_units() > 0) nx::ram::set_u16(CRYSTALS, static_cast<uint16_t>(max_crystal_units()));
        if (max_film() > 0) nx::ram::set_u16(FILM, static_cast<uint16_t>(max_film()));
        for (int kong = 0; kong < KONG_COUNT; kong++) {
            nx::ram::set_u16(kong_base(kong) + INSTRUMENT_ENERGY, static_cast<uint16_t>(max_instrument_energy(kong)));
        }
        nx::ram::set_u8(HEALTH, static_cast<uint8_t>(nx::ram::u8(MELONS) * 4));
    }

    // A button's text: "Done" for a moment after it ran, "Not in game" outside a single-player adventure.
    uint64_t moves_tick = 0, refill_tick = 0;
    std::string button_text(const char* text, uint64_t done_tick) {
        if (!in_adventure()) return "Not in game";
        const bool recent = done_tick != 0 && armTicksToNs(armGetSystemTick() - done_tick) < 1'500'000'000ull;
        return recent ? std::string("Done") : std::string(text);
    }

    // CPU, GPU and RAM clocks as the console runs them (clkrst), read once a second while the menu is open.
    std::string clocks_text = "-";

    // Interpolation over the last second.
    uint64_t interp_window[4] = {};
    uint64_t interp_window_ms = 0;
    double interp_share = 0.0, interp_cuts = 0.0, interp_unmatched = 0.0;

    std::string map_text() {
        const uint32_t map = nx::ram::u32(CURRENT_MAP);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "0x%02X  %s", map, map < static_cast<uint32_t>(DK64_MAP_COUNT) ? DK64_MAP_NAMES[map] : "?");
        return std::string(buffer);
    }

    std::string position_text() {
        const uint32_t actor = nx::ram::u32(PLAYER_POINTER);
        if (!nx::ram::valid(actor)) return std::string("-");
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.0f, %.0f, %.0f", nx::ram::f32(actor + ACTOR_POSITION),
                      nx::ram::f32(actor + ACTOR_POSITION + 4), nx::ram::f32(actor + ACTOR_POSITION + 8));
        return std::string(buffer);
    }

    bool overlay = false;

    const nx::dev::Tool TOOLS[] = {
        {"Game", "Map", "The map the game is in: its ID and its name in the DK64 decomp.", map_text, nullptr},
        {"Game", "Kong", "The character the player controls.",
         [] {
             const int kong = current_kong();
             return std::string(kong < 8 ? KONG_NAMES[kong] : "?");
         }, nullptr},
        {"Game", "Position", "The player's x, y and z in the map's units.", position_text, nullptr},
        {"Game", "Map & Position Overlay",
         "Shows the map and the player's position in the bottom left corner while playing, as the Paper Mario "
         "port's Value Viewer does.",
         [] { return std::string(overlay ? "On" : "Off"); }, [](int, bool) { overlay = !overlay; }},

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
             nx::ram::set_u16(STANDARD_AMMO, static_cast<uint16_t>(stepped(nx::ram::u16(STANDARD_AMMO), direction, wrap, 10, 0, max_ammo())));
         }},
        {"Inventory", "Oranges", "Orange grenades.",
         [] { return std::to_string(nx::ram::u16(ORANGES)); },
         [](int direction, bool wrap) {
             nx::ram::set_u16(ORANGES, static_cast<uint16_t>(stepped(nx::ram::u16(ORANGES), direction, wrap, 1, 0, max_oranges())));
         }},
        {"Inventory", "Crystal Coconuts", "Crystals that power the Kongs' special moves.",
         [] { return std::to_string(nx::ram::u16(CRYSTALS) / CRYSTAL_UNITS); },
         [](int direction, bool wrap) {
             const int most = max_crystal_units() > 0 ? max_crystal_units() / CRYSTAL_UNITS : 20;  // 20 before the camera
             const int crystals = stepped(nx::ram::u16(CRYSTALS) / CRYSTAL_UNITS, direction, wrap, 1, 0, most);
             nx::ram::set_u16(CRYSTALS, static_cast<uint16_t>(crystals * CRYSTAL_UNITS));
         }},
        {"Inventory", "Film", "Film for the Banana Fairy camera.",
         [] { return std::to_string(nx::ram::u16(FILM)); },
         [](int direction, bool wrap) {
             const int most = max_film() > 0 ? max_film() : 20;  // 20 before the camera
             nx::ram::set_u16(FILM, static_cast<uint16_t>(stepped(nx::ram::u16(FILM), direction, wrap, 1, 0, most)));
         }},
        {"Unlocks", "Give All Moves",
         "Every Kong learns all its moves, gun upgrades, the largest ammo belt and its instrument, plus the training "
         "barrels' moves and the camera, as the game's own debug code does. Story scenes that teach them may then "
         "look odd. Save from the pause menu to keep them.",
         [] { return button_text("Give", moves_tick); },
         [](int, bool wrap) {
             if (!wrap || !in_adventure()) return;
             give_all_moves();
             moves_tick = armGetSystemTick();
         }, true},
        {"Unlocks", "Refill Consumables",
         "Ammo, homing ammo, oranges, crystal coconuts, film, every Kong's instrument energy and health, to the most "
         "the current upgrades hold (crystals and film need the camera).",
         [] { return button_text("Refill", refill_tick); },
         [](int, bool wrap) {
             if (!wrap || !in_adventure()) return;
             refill_consumables();
             refill_tick = armGetSystemTick();
         }, true},
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
         "Above 30 fps, the share of the game's frames that got an in-between image over the last second: the chosen "
         "rate's share at most (all at 60). Camera cuts, loading screens and frames too slow to draw twice get none.",
         [] { return nx::interpolating() ? text("%.0f%%", interp_share) : std::string("30 fps"); }, nullptr},
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

bool nx::dev::overlay_shown() { return overlay; }

extern "C" uint64_t osGetTime();  // ultramodern timer.cpp

long nx::dev::play_time_seconds() {
    if (!in_adventure()) return -1;
    const auto ostime = [](uint32_t address) { return uint64_t{nx::ram::u32(address)} << 32 | nx::ram::u32(address + 4); };
    const bool paused = (nx::ram::u32(GLOBAL_PROPERTIES) & 2) != 0;
    const uint64_t now = paused ? ostime(PAUSE_START) : osGetTime();
    const uint64_t start = ostime(SESSION_START);
    const uint64_t elapsed = now > start ? now - start : 0;
    return static_cast<long>(nx::ram::u32(SAVED_PLAY_SECONDS) + elapsed / OSTIME_PER_SECOND);
}
std::string nx::dev::overlay_text() { return map_text() + "    " + position_text(); }

void nx::dev::request_warp(int map, int exit) {
    pending_warp = map << 8 | exit;
}

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
