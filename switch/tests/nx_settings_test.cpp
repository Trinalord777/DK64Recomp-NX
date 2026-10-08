// Host check of nx_settings.cpp: c++ -std=c++20 -DNX_SETTINGS_PATH='"/tmp/x.txt"' nx_settings.cpp tests/nx_settings_test.cpp
#include <cassert>
#include <cstdio>

#include "../nx_settings.h"

using nx::Setting;

int main() {
    // Missing file: defaults.
    std::remove(NX_SETTINGS_PATH);
    nx::load();
    assert(nx::get(Setting::MusicVolume) == 100);
    assert(nx::get(Setting::CutsceneBorders) == 1);
    assert(nx::resolution_factor() == 3 && nx::widescreen());
    assert(nx::get(Setting::FrameRate) == 30 && nx::get(Setting::ClockProfile) == 0);

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
    assert(nx::get(Setting::MusicVolume) == 40 && nx::get(Setting::StorySkip) == 2);
    std::remove(NX_SETTINGS_PATH);
    std::puts("nx_settings: OK");
    return 0;
}
