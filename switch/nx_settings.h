// Options of the in-game "NX Options" menu (nx_menu.cpp), saved to sdmc:/switch/dk64recomp/nx_options.txt.
// The game reads them through the dk64::get_* getters (frontend_switch.cpp), so most apply immediately;
// the display ones are read once at startup.
#pragma once

#include <initializer_list>

namespace nx {
    enum class Setting {
        CameraType,
        CameraSensitivity,
        InvertCamera,
        InvertFirstPerson,
        InvertSwimming,
        StorySkip,
        LightningFlashes,
        CutsceneBorders,
        DrawDistance,
        MusicVolume,
        SoundVolume,
        ShowFps,
        AspectRatio,
        Resolution,
        TextureLod,
        FrameRate,
        ClockProfile,
        FaceButtons,
        PerformanceOverlay,
        MenuTheme,
        MenuOpacity,
        MenuSize,
        MasterVolume,
        TextureFilter,
        InputViewer,
        GameplayTimer,
        FrameMode,
        Count
    };

    struct SettingInfo {
        const char* section;
        const char* key;          // name in nx_options.txt
        const char* label;
        const char* description;
        int default_value;
        // A choice setting stores the index of its choice; a number setting a value in [min, max].
        std::initializer_list<const char*> choices;
        int min = 0;
        int max = 0;
        int step = 0;
        bool needs_restart = false;
        const char* suffix = "";  // after a number value, e.g. "%"
    };

    const SettingInfo& info(Setting setting);
    int get(Setting setting);
    // Clamps to the setting's range.
    void set(Setting setting, int value);
    // Defaults, then the saved file: call once at startup, before the game reads any setting.
    void load();
    void save();
    // Value at startup, for the settings that need a restart: what the renderer is actually using.
    int at_startup(Setting setting);

    inline int resolution_factor() { return at_startup(Setting::Resolution) + 2; }
    // 4x (experimental) draws into a 1920x1080 window instead of 1280x720.
    inline bool output_1080p() { return at_startup(Setting::Resolution) == 2; }
    inline int screen_width() { return output_1080p() ? 1920 : 1280; }
    inline int screen_height() { return output_1080p() ? 1080 : 720; }
    inline bool widescreen() { return at_startup(Setting::AspectRatio) == 0; }
    // Frames shown per second: 30 is the game's own rate; above it, some of its frames get an in-between image
    // (all of them at 60).
    inline int target_fps() { return get(Setting::FrameRate); }
    // Above 30 fps: in-between images (the game keeps its 30 steps per second), or, in the True mode, frames of the
    // game itself, as fast as the chosen rate.
    inline bool above_30() { return target_fps() > 30; }
    inline bool true_frames() { return above_30() && get(Setting::FrameMode) == 1; }
    inline bool interpolating() { return above_30() && !true_frames(); }
}
