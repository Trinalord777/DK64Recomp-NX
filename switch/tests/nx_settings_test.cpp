// Host check of nx_settings.cpp: c++ -std=c++20 -DNX_SETTINGS_PATH='"/tmp/x.txt"' nx_settings.cpp tests/nx_settings_test.cpp
#include <cassert>
#include <cstring>
#include <cstdio>
#include <utility>

#include "../nx_settings.h"

using nx::Setting;

int main() {
    // Missing file: defaults.
    std::remove(NX_SETTINGS_PATH);
    nx::load();
    assert(nx::get(Setting::MusicVolume) == 100);
    assert(nx::get(Setting::CutsceneBorders) == 1);
    assert(nx::resolution_factor() == 3 && nx::widescreen());
    assert(nx::get(Setting::FrameRate) == 30 && !nx::interpolating() && nx::get(Setting::ClockProfile) == 0);

    // Valid, malformed, unknown and out-of-range lines.
    FILE* file = std::fopen(NX_SETTINGS_PATH, "w");
    std::fputs("music_volume = 40\n  camera_type=3\ngarbage\nnot_a_key = 5\nsound_volume = 250\n"
               "resolution = 0\ninvert_camera = -4\nclock_profile = 2\ndraw_distance = 4294967300\n", file);
    std::fclose(file);
    nx::load();
    assert(nx::get(Setting::MusicVolume) == 40);
    assert(nx::get(Setting::CameraType) == 3);
    assert(nx::get(Setting::SoundVolume) == 100);   // clamped
    assert(nx::get(Setting::InvertCamera) == 0);    // clamped
    assert(nx::resolution_factor() == 2);
    assert(nx::get(Setting::ClockProfile) == 2);
    assert(nx::get(Setting::DrawDistance) == 100);  // overflow saturates, then clamps

    // Changes while running do not move the startup snapshot; save + load round-trips.
    nx::set(Setting::Resolution, 1);
    nx::set(Setting::StorySkip, 9);
    assert(nx::get(Setting::StorySkip) == 2);
    assert(nx::resolution_factor() == 2);
    nx::save();
    FILE* temporary = std::fopen(NX_SETTINGS_PATH ".tmp", "r");
    assert(temporary == nullptr);
    nx::load();
    assert(nx::get(Setting::Resolution) == 1 && nx::resolution_factor() == 3);
    assert(!nx::output_1080p() && nx::screen_width() == 1280 && nx::screen_height() == 720);
    // The experimental 4x draws 1280x960 into a 1080p window.
    nx::set(Setting::Resolution, 2);
    nx::save();
    nx::load();
    assert(nx::resolution_factor() == 4 && nx::output_1080p() && nx::screen_width() == 1920 && nx::screen_height() == 1080);
    nx::set(Setting::Resolution, 9);
    assert(nx::get(Setting::Resolution) == 2);  // clamped to the last choice
    nx::set(Setting::Resolution, 1);
    nx::save();
    nx::load();
    assert(nx::get(Setting::MusicVolume) == 40 && nx::get(Setting::StorySkip) == 2);

    // The frame rate saved before the 30-60 range: frame_rate 1 meant 60 fps, 0 meant 30; the new key wins.
    for (const auto& [lines, fps] : {std::pair{"frame_rate = 1\n", 60}, std::pair{"frame_rate = 0\n", 30},
                                     std::pair{"frame_rate = 1\nframe_rate_fps = 45\n", 45},
                                     std::pair{"frame_rate_fps = 75\n", 60}}) {
        FILE* legacy = std::fopen(NX_SETTINGS_PATH, "w");
        std::fputs(lines, legacy);
        std::fclose(legacy);
        nx::load();
        assert(nx::target_fps() == fps);
    }

    // MenuTheme defaults to 0 (Orange) and round-trips.
    std::remove(NX_SETTINGS_PATH);
    nx::load();
    assert(nx::get(Setting::MenuTheme) == 0);
    nx::set(Setting::MenuTheme, 1);
    assert(nx::get(Setting::MenuTheme) == 1);
    nx::save();
    nx::load();
    assert(nx::get(Setting::MenuTheme) == 1);
    std::remove(NX_SETTINGS_PATH);

    // SETTINGS[] is indexed by the enum: an entry out of order made each setting read its neighbour's value (the
    // Input Viewer switch drove the Frame Rate Mode). Every key must sit at its enum's index.
    const std::pair<Setting, const char*> keys[] = {
        {Setting::Resolution, "resolution"}, {Setting::FrameRate, "frame_rate_fps"},
        {Setting::ClockProfile, "clock_profile"}, {Setting::MenuTheme, "menu_theme"},
        {Setting::MenuOpacity, "menu_opacity"}, {Setting::MasterVolume, "master_volume"},
        {Setting::TextureFilter, "texture_filter"}, {Setting::InputViewer, "input_viewer"},
        {Setting::GameplayTimer, "gameplay_timer"}, {Setting::FrameMode, "frame_mode"}};
    for (const auto& [setting, key] : keys) assert(std::strcmp(nx::info(setting).key, key) == 0);

    std::puts("nx_settings: OK");
    return 0;
}
