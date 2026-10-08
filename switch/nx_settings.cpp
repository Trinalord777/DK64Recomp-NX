#include "nx_settings.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
#ifndef NX_SETTINGS_PATH
#define NX_SETTINGS_PATH "sdmc:/switch/dk64recomp/nx_options.txt"
#endif
    constexpr const char* SETTINGS_PATH = NX_SETTINGS_PATH;
    constexpr int COUNT = static_cast<int>(nx::Setting::Count);
    constexpr std::initializer_list<const char*> INVERT = {"None", "Invert X", "Invert Y", "Invert Both"};
    constexpr std::initializer_list<const char*> OFF_ON = {"Off", "On"};

    // Order and values follow nx::Setting and the dk64 enums (include/donk_config.h); labels and defaults
    // follow the PC port (src/game/config.cpp), except the cutscene borders, off as in v1.0.
    const nx::SettingInfo SETTINGS[COUNT] = {
        {"Camera", "camera_type", "Camera Type",
         "Free: C-Left/C-Right turn the camera. Follow: it also swings behind DK. Better Free: holding the "
         "button turns it smoothly. Analog: the right stick moves the camera.",
         0, {"Free Cam", "Follow Cam", "Better Free Cam", "Analog Camera"}},
        {"Camera", "camera_sensitivity", "Analog Camera Sensitivity",
         "Speed of the right stick camera (Camera Type: Analog Camera).", 3, {}, 1, 10, 1},
        {"Camera", "invert_camera", "Invert Camera", "Inverts the third person camera controls.", 0, INVERT},
        {"Camera", "invert_first_person", "Invert First Person View",
         "Inverts first person aiming. Invert Y matches the original game.", 2, INVERT},
        {"Camera", "invert_swimming", "Invert Swimming",
         "Inverts the swimming controls. Invert Y matches the original game.", 2, INVERT},
        {"Game", "story_skip", "Story Skip",
         "Intro Story Only skips the 5-minute introduction. On also skips the K. Rool level intros.",
         0, {"Off", "Intro Story Only", "On"}},
        {"Game", "lightning_flashes", "Lightning Flashes",
         "Reduced flashes 10 times less often than the original game.", 1, {"Vanilla", "Reduced", "Off"}},
        {"Game", "cutscene_borders", "Cutscene Borders",
         "Black bars at the top and bottom of the screen during cutscenes, as in the original game.", 1,
         {"On", "Off"}},
        {"Game", "draw_distance", "Draw Distance",
         "From 0% (original) to 100% (10 times the original). Costs performance in large levels.",
         0, {}, 0, 100, 10, false, "%"},
        {"Audio", "music_volume", "Music Volume", "Background music volume.", 100, {}, 0, 100, 5, false, "%"},
        {"Audio", "sound_volume", "Sound Effects Volume", "Sound effects volume.", 100, {}, 0, 100, 5, false, "%"},
        {"Display", "show_fps", "Show FPS", "Frames shown per second, in the top left corner.", 0, OFF_ON},
        {"Display", "aspect_ratio", "Aspect Ratio",
         "16:9 fills the screen. 4:3 shows the original framing with black bars on the sides.",
         0, {"16:9", "4:3"}, 0, 0, 0, true},
        {"Display", "resolution", "Resolution",
         "Internal resolution. 2x is lighter on the console, 3x fills the height of the 720p screen.",
         1, {"2x (640x480)", "3x (960x720)"}, 0, 0, 0, true},
        {"Performance", "frame_rate", "Frame Rate",
         "30 is the original. Above 30 (experimental) the game mixes frames of one and two screen refreshes, as it "
         "does in Jetpac, and stops waiting for the screen. Some timers and mini-games may run too fast.",
         30, {}, 30, 60, 5, false, " fps"},
        {"Performance", "clock_profile", "Clock Profile",
         "Handheld only, while in the game. Docked GPU: the official docked GPU profile (768 MHz). Horizon OC: "
         "the docked CPU, GPU and RAM clocks set in Horizon OC.",
         0, {"Console Default", "Docked GPU in Handheld", "Horizon OC Docked Clocks"}},
    };

    std::atomic<int> values[COUNT];
    int startup_values[COUNT];

    int index(nx::Setting setting) { return static_cast<int>(setting); }

    int clamp(const nx::SettingInfo& info, int value) {
        if (info.choices.size() > 0) {
            return std::clamp(value, 0, static_cast<int>(info.choices.size()) - 1);
        }
        return std::clamp(value, info.min, info.max);
    }
}

const nx::SettingInfo& nx::info(Setting setting) { return SETTINGS[index(setting)]; }

int nx::get(Setting setting) { return values[index(setting)].load(std::memory_order_relaxed); }

void nx::set(Setting setting, int value) {
    values[index(setting)].store(clamp(info(setting), value), std::memory_order_relaxed);
}

// "key = value" lines; unknown keys and malformed lines are skipped, out-of-range values clamped.
void nx::load() {
    for (int i = 0; i < COUNT; i++) values[i] = SETTINGS[i].default_value;
    if (FILE* file = std::fopen(SETTINGS_PATH, "r")) {
        char line[128];
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            char key[64];
            char text[16];
            if (std::sscanf(line, " %63[a-z_] = %15s", key, text) != 2) continue;
            char* end = nullptr;
            // strtol saturates instead of overflowing like %d.
            const long value = std::strtol(text, &end, 10);
            if (end == text) continue;
            for (int i = 0; i < COUNT; i++) {
                if (std::strcmp(key, SETTINGS[i].key) == 0) {
                    set(static_cast<Setting>(i), static_cast<int>(std::clamp(value, -1000L, 1000L)));
                }
            }
        }
        std::fclose(file);
    }
    for (int i = 0; i < COUNT; i++) startup_values[i] = values[i];
}

int nx::at_startup(Setting setting) { return startup_values[index(setting)]; }

// Written next to the file, then renamed over it, so a full card or a power cut never leaves it truncated.
void nx::save() {
    const std::string temporary = std::string(SETTINGS_PATH) + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "w");
    bool written = file != nullptr;
    for (int i = 0; written && i < COUNT; i++) {
        written = std::fprintf(file, "%s = %d\n", SETTINGS[i].key, values[i].load(std::memory_order_relaxed)) > 0;
    }
    if (file != nullptr && std::fclose(file) != 0) written = false;
    // The SD card's rename does not replace an existing file.
    if (!written || (std::remove(SETTINGS_PATH) != 0 && errno != ENOENT) ||
        std::rename(temporary.c_str(), SETTINGS_PATH) != 0) {
        std::fprintf(stderr, "[DK64NX] cannot write %s\n", SETTINGS_PATH);
        std::remove(temporary.c_str());
    }
}
