// Switch replacements for the recompui calls and the dk64 config getters.
// The PC port reads these options from RmlUi menus; here they come from the NX Options menu (nx_settings.cpp).
#include <cstdio>

#include <SDL.h>
#include <switch.h>

#include "donk_config.h"
#include "nx_settings.h"
#include "recomp.h"
#include "donk_draw.h"
#include "donk_sound.h"
#include "recompui/recompui.h"
#include "recompui/renderer.h"

extern SDL_Window* dk64nx_window;

void recompui::message_box(const char* msg) {
    fprintf(stderr, "[DK64NX] %s\n", msg);
    ErrorApplicationConfig config;
    errorApplicationCreate(&config, "DK64: Recompiled", msg);
    errorApplicationShow(&config);
}

void recompui::get_window_size(int& width, int& height) {
    if (dk64nx_window == nullptr) {
        width = 1280;
        height = 720;
        return;
    }
    SDL_GetWindowSize(dk64nx_window, &width, &height);
}

bool recompui::renderer::RT64HighPrecisionFBEnabled() {
    return false;
}

void dk64::init_config() {}

void dk64::open_quit_game_prompt() {}

namespace {
    template <typename T>
    T setting(nx::Setting id) { return static_cast<T>(nx::get(id)); }
}

// Both third person camera getters follow Invert Camera, as the PC menu shows a single option.
dk64::CameraInvertMode dk64::get_camera_invert_mode() { return setting<CameraInvertMode>(nx::Setting::InvertCamera); }
dk64::CameraInvertMode dk64::get_third_person_camera_mode() { return setting<CameraInvertMode>(nx::Setting::InvertCamera); }
dk64::CameraInvertMode dk64::get_swimming_invert_mode() { return setting<CameraInvertMode>(nx::Setting::InvertSwimming); }
dk64::CameraInvertMode dk64::get_first_person_invert_mode() { return setting<CameraInvertMode>(nx::Setting::InvertFirstPerson); }
dk64::CameraInvertMode dk64::get_gyro_invert_mode() { return CameraInvertMode::InvertY; }
dk64::CameraInvertMode dk64::get_mouse_invert_mode() { return CameraInvertMode::InvertY; }
uint32_t dk64::get_analog_cam_sensitivity() { return setting<uint32_t>(nx::Setting::CameraSensitivity); }
dk64::StorySkipMode dk64::get_story_skip() { return setting<StorySkipMode>(nx::Setting::StorySkip); }
dk64::CameraTypeMode dk64::get_camera_type() { return setting<CameraTypeMode>(nx::Setting::CameraType); }
dk64::LightningFlashMode dk64::get_lightning_flash() { return setting<LightningFlashMode>(nx::Setting::LightningFlashes); }
dk64::CutsceneBordersMode dk64::get_cutscene_borders() { return setting<CutsceneBordersMode>(nx::Setting::CutsceneBorders); }
dk64::MultiplayerEnabled dk64::get_multiplayer_enabled() { return MultiplayerEnabled::Off; }
int dk64::get_bgm_volume() { return nx::get(nx::Setting::MusicVolume); }
int dk64::get_sfx_volume() { return nx::get(nx::Setting::SoundVolume); }
int dk64::get_draw_distance() { return nx::get(nx::Setting::DrawDistance); }

// Pumps RmlUi callbacks registered by mods; there is no UI (nor mods) on Switch.
extern "C" void recomp_run_ui_callbacks(uint8_t*, recomp_context*) {}
