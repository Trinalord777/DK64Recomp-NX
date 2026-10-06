// Switch replacements for the recompui calls and the dk64 config getters.
// The PC port reads these options from RmlUi menus; here they are the upstream defaults
// (src/game/config.cpp), so the game behaves like a fresh PC install.
#include <cstdio>

#include <SDL.h>
#include <switch.h>

#include "donk_config.h"
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

dk64::CameraInvertMode dk64::get_camera_invert_mode() { return CameraInvertMode::InvertNone; }
dk64::CameraInvertMode dk64::get_third_person_camera_mode() { return CameraInvertMode::InvertNone; }
dk64::CameraInvertMode dk64::get_swimming_invert_mode() { return CameraInvertMode::InvertY; }
dk64::CameraInvertMode dk64::get_first_person_invert_mode() { return CameraInvertMode::InvertY; }
dk64::CameraInvertMode dk64::get_gyro_invert_mode() { return CameraInvertMode::InvertY; }
dk64::CameraInvertMode dk64::get_mouse_invert_mode() { return CameraInvertMode::InvertY; }
uint32_t dk64::get_analog_cam_sensitivity() { return 3; }
dk64::StorySkipMode dk64::get_story_skip() { return StorySkipMode::Off; }
dk64::CameraTypeMode dk64::get_camera_type() { return CameraTypeMode::Free; }
dk64::LightningFlashMode dk64::get_lightning_flash() { return LightningFlashMode::Reduced; }
// Off: no letterbox in cutscenes, the 16:9 image fills the screen (JT's request).
dk64::CutsceneBordersMode dk64::get_cutscene_borders() { return CutsceneBordersMode::Off; }
dk64::MultiplayerEnabled dk64::get_multiplayer_enabled() { return MultiplayerEnabled::Off; }
int dk64::get_bgm_volume() { return 100; }
int dk64::get_sfx_volume() { return 100; }
int dk64::get_draw_distance() { return 0; }

// Pumps RmlUi callbacks registered by mods; there is no UI (nor mods) on Switch.
extern "C" void recomp_run_ui_callbacks(uint8_t*, recomp_context*) {}
