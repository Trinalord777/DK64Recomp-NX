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
        FrameRate,
        ClockProfile,
        FaceButtons,
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

    inline int resolution_factor() { return at_startup(Setting::Resolution) == 0 ? 2 : 3; }
    inline bool widescreen() { return at_startup(Setting::AspectRatio) == 0; }
    inline bool interpolated_60() { return get(Setting::FrameRate) == 1; }
}
