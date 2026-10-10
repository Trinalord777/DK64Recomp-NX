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
    // Before the 30-60 range, the frame rate was a choice saved as frame_rate: 0 for 30 fps, 1 for 60.
    constexpr const char* LEGACY_FRAME_RATE = "frame_rate";

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
        {"Gameplay", "story_skip", "Story Skip",
         "Intro Story Only skips the 5-minute introduction. On also skips the K. Rool level intros.",
         0, {"Off", "Intro Story Only", "On"}},
        {"Gameplay", "lightning_flashes", "Lightning Flashes",
         "Reduced flashes 10 times less often than the original game.", 1, {"Vanilla", "Reduced", "Off"}},
        {"Gameplay", "cutscene_borders", "Cutscene Borders",
         "Black bars at the top and bottom of the screen during cutscenes, as in the original game.", 1,
         {"On", "Off"}},
        {"Gameplay", "draw_distance", "Draw Distance",
         "From 0% (original) to 100% (10 times the original). Costs performance in large levels.",
         0, {}, 0, 100, 10, false, "%"},
        {"Audio", "music_volume", "Music Volume", "Background music volume.", 100, {}, 0, 100, 5, false, "%"},
        {"Audio", "sound_volume", "Sound Effects Volume", "Sound effects volume.", 100, {}, 0, 100, 5, false, "%"},
        {"Overlays", "show_fps", "Show FPS", "Frames shown per second, in the top left corner.", 0, OFF_ON},
        {"Display", "aspect_ratio", "Aspect Ratio",
         "16:9 fills the screen. 4:3 shows the original framing with black bars on the sides.",
         0, {"16:9", "4:3"}, 0, 0, 0, true},
        {"Display", "resolution", "Resolution",
         "Internal resolution. 2x is lighter on the console, 3x fills the height of the 720p screen.",
         1, {"2x (640x480)", "3x (960x720)"}, 0, 0, 0, true},
        {"Display", "texture_lod", "Texture LOD",
         "GLideN64's level of detail emulation. On blurs the Kongs' eyes and faces and tints them pink (as reported "
         "in mupen64plus-libretro-nx issue 458); Off draws them sharp.",
         0, OFF_ON, 0, 0, 0, true},
        {"Performance", "frame_rate_fps", "Frame Rate",
         "Frames shown per second. The game runs at its own 30; above that, an in-between image is drawn between two "
         "of its frames, as on PC: for 1 frame in 6 at 35, every other one at 45, all at 60. On a 60 Hz screen the "
         "rates between show smooth and held images in turn. Costs one frame of latency.",
         30, {}, 30, 60, 5, false, " fps"},
        {"Performance", "clock_profile", "Clock Profile",
         "Clocks while in the game. Docked GPU (handheld only): the official 768 MHz GPU profile. Horizon OC "
         "(handheld only): the docked clocks set in Horizon OC. CPU Boost (needs Horizon OC): the CPU at 1785 MHz, "
         "the official boost clock, docked and handheld.",
         0, {"Console Default", "Docked GPU in Handheld", "Horizon OC Docked Clocks", "CPU Boost (Horizon OC)"}},
        {"Controls", "face_buttons", "Face Buttons",
         "Flipped swaps the A and B buttons in the game (not in this menu).",
         0, {"Normal", "Flipped"}},
        {"Overlays", "performance_overlay", "Performance Overlay",
         "Top left: frames shown per second, the time of each pass of a frame (two at 60 fps: the real image and "
         "the in-between one) and the screen refreshes that showed nothing new. Below 16.7 ms per frame, 60 fps holds.",
         0, OFF_ON},
        {"Menu", "menu_theme", "Menu Theme",
         "Colors of this menu: orange or gray, after DK64's own menus.",
         0, {"Orange", "Gray"}},
        {"Menu", "menu_opacity", "Menu Opacity", "How much of the game shows through this menu: less is more.",
         95, {}, 50, 100, 5, false, "%"},
        {"Menu", "menu_size", "Menu Size", "Size of the text and the boxes of this menu.", 1, {"Small", "Medium", "Large"}},
        {"Audio", "master_volume", "Master Volume", "Volume of everything the game plays, music and sound effects.",
         100, {}, 0, 100, 5, false, "%"},
        {"Display", "texture_filter", "Texture Filter",
         "How textures are smoothed: Standard bilinear filtering, or the N64's own 3-point filter, sharper and closer "
         "to the console. Every shader is compiled again at the next start.",
         0, {"Standard", "N64 3-Point"}, 0, 0, 0, true},
        {"Overlays", "input_viewer", "Input Viewer",
         "The buttons and sticks being pressed, in the bottom right corner, as the HarbourMasters ports show them.",
         0, OFF_ON},
        {"Overlays", "gameplay_timer", "Gameplay Timer",
         "The file's play time, the one the file select shows, in the top right corner while playing an adventure.",
         0, OFF_ON},
        {"Performance", "frame_mode", "Frame Rate Mode",
         "Above 30 fps. Interpolated: the game keeps its 30 steps per second and in-between images are drawn, as on PC. "
         "True (experimental): the game itself runs up to the chosen rate, as DK64 does in Jetpac. DK runs at the right "
         "speed, but much of the game (animations, enemies, timers) runs faster than intended. The DK Rap and the intros "
         "stay at 30.",
         0, {"Interpolated", "True (experimental)"}},
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
    bool legacy_60 = false;
    bool rate_saved = false;
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
            if (std::strcmp(key, LEGACY_FRAME_RATE) == 0) legacy_60 = value == 1;
            for (int i = 0; i < COUNT; i++) {
                if (std::strcmp(key, SETTINGS[i].key) == 0) {
                    set(static_cast<Setting>(i), static_cast<int>(std::clamp(value, -1000L, 1000L)));
                    rate_saved = rate_saved || static_cast<Setting>(i) == Setting::FrameRate;
                }
            }
        }
        std::fclose(file);
    }
    if (legacy_60 && !rate_saved) set(Setting::FrameRate, 60);
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
