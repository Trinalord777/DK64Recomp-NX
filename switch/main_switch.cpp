// Switch entry point for DK64: Recompiled. Mirrors src/main/main.cpp without the PC frontend
// (RmlUi launcher, RT64, native file dialogs, texture packs): the game boots straight from the
// ROM placed in sdmc:/switch/dk64recomp/.
#include <algorithm>
#include <array>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <exception>
#include <vector>

#include <unistd.h>

#include <SDL.h>
#include <switch.h>

#include "donk_config.h"
#include "donk_game.h"
#include "nx_cheats.h"
#include "nx_settings.h"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"
#include "ovl_patches.hpp"
#include "recomp_data.h"
#include "recompinput/recompinput.h"
#include "recompui/recompui.h"
#include "ultramodern/config.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

#include "../patches/graphics.h"
#include "../patches/input.h"
#include "../patches/misc_funcs.h"
#include "../patches/options.h"
#include "../patches/sound.h"
#include "../patches/ui.h"

SDL_Window* dk64nx_window = nullptr;

// nvdrv keeps the bookkeeping of every GPU allocation (mesa makes one nvMapCreate per texture) in the transfer memory
// libnx hands it at startup: 8 MiB by default (a weak symbol). Measured on the console, uploads start failing with
// GL_OUT_OF_MEMORY near 5,000 live textures; GLideN64 caches up to 8,000, so a long session ended in black and noisy
// textures until a restart. 32 MiB holds about 20,000 allocations.
extern "C" u32 __nx_nv_transfermem_size;
u32 __nx_nv_transfermem_size = 32 * 1024 * 1024;

void dk64nx_input_handle_event(const SDL_Event& event);
void dk64nx_perf_start();
void dk64nx_perf_update();
void dk64nx_perf_stop();
extern "C" void dk64nx_jit_close();
extern "C" void dk64nx_window_set_size(int width, int height);  // window_switch.c
#ifdef DK64NX_REMOTE
void dk64nx_remote_start();
void dk64nx_remote_stop();
extern "C" void dk64nx_trace(const char* what);
#endif
bool dk64nx_get_input(int controller_num, uint16_t* buttons, float* x, float* y);
ultramodern::input::connected_device_info_t dk64nx_get_connected_device_info(int controller_num);
std::unique_ptr<ultramodern::renderer::RendererContext> dk64nx_create_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();
extern RspUcodeFunc n_aspMain;

// Milliseconds since the console booted, for the start-up timeline in the log (also core_switch.cpp).
extern "C" unsigned long long dk64nx_ms_since_boot() { return armTicksToNs(armGetSystemTick()) / 1000000; }

namespace {
    const std::filesystem::path DATA_DIR = "sdmc:/switch/dk64recomp";
    const std::u8string GAME_ID = u8"DK64";
    // librecomp's --game switch looks games up by their mod id.
    char MOD_GAME_ID[] = "dk64";
    const std::string VERSION_STRING = "1.0.3";

    // With the time since the console booted, to see where the start of the game goes.
    void step(const char* what) {
        printf("[DK64NX] step: %s (%llu ms)\n", what, dk64nx_ms_since_boot());
    }

    [[noreturn]] void log_terminate() {
        try {
            if (auto current = std::current_exception()) std::rethrow_exception(current);
            fprintf(stderr, "[DK64NX] std::terminate without an active exception\n");
        } catch (const std::exception& e) {
            fprintf(stderr, "[DK64NX] uncaught exception: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "[DK64NX] uncaught non-standard exception\n");
        }
        svcSleepThread(2000000000ULL);
        std::abort();
    }

    void init_logging() {
        std::filesystem::create_directories(DATA_DIR);
        socketInitializeDefault();
        if (nxlinkStdio() >= 0) {
            setvbuf(stdout, nullptr, _IONBF, 0);
            setvbuf(stderr, nullptr, _IONBF, 0);
            return;
        }
#ifndef DK64NX_REMOTE
        socketExit();
#endif
        // Launched from hbmenu: keep a log on the SD card instead.
        const std::string log_path = (DATA_DIR / "dk64recomp.log").string();
        // The previous run's log survives one launch: after a crash, relaunching would otherwise erase its trace.
        const std::string previous_path = (DATA_DIR / "dk64recomp.prev.log").string();
        std::remove(previous_path.c_str());
        std::rename(log_path.c_str(), previous_path.c_str());
        freopen(log_path.c_str(), "w", stdout);
        freopen(log_path.c_str(), "a", stderr);
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }

    bool is_rom_extension(std::string ext) {
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        return ext == ".z64" || ext == ".n64" || ext == ".v64";
    }

    // librecomp keeps a validated, byteswapped copy as <config>/DK64.z64. On first boot, import
    // whichever dump the user dropped next to it.
    bool ensure_rom() {
        recomp::check_all_stored_roms();
        if (recomp::is_rom_valid(GAME_ID)) {
            return true;
        }
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(DATA_DIR, ec)) {
            if (!entry.is_regular_file() || !is_rom_extension(entry.path().extension().string())) {
                continue;
            }
            auto result = recomp::select_rom(entry.path(), GAME_ID);
            printf("[DK64NX] ROM %s -> validation %d\n", entry.path().c_str(), static_cast<int>(result));
            if (result == recomp::RomValidationError::Good) {
                return true;
            }
        }
        ultramodern::error_handling::message_box(
            "No valid Donkey Kong 64 ROM found.\n"
            "Copy your NTSC-U (USA) dump (.z64/.n64/.v64) to sdmc:/switch/dk64recomp/");
        return false;
    }

    // Original framerate. In 16:9, Expand + Full HUD, as on PC: DK64R then sizes its UI and screen effects for
    // the 16:9 window (recomp_get_ui_bounds = 426 wide), which GLideN64's adjust mode lays out like RT64.
    void set_switch_graphics_config() {
        ultramodern::renderer::GraphicsConfig config{};
        config.developer_mode = false;
        config.res_option = ultramodern::renderer::Resolution::Original;
        config.wm_option = ultramodern::renderer::WindowMode::Fullscreen;
        config.hr_option = ultramodern::renderer::HUDRatioMode::Full;
        config.api_option = ultramodern::renderer::GraphicsApi::Auto;
        config.ar_option = nx::widescreen() ? ultramodern::renderer::AspectRatio::Expand
                                            : ultramodern::renderer::AspectRatio::Original;
        config.msaa_option = ultramodern::renderer::Antialiasing::None;
        config.rr_option = ultramodern::renderer::RefreshRate::Original;
        config.hpfb_option = ultramodern::renderer::HighPrecisionFramebuffer::Off;
        config.rr_manual_value = 30;
        config.ds_option = 1;
        ultramodern::renderer::set_graphics_config(config);
    }

    ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != 0) {
            fprintf(stderr, "[DK64NX] SDL_Init failed: %s\n", SDL_GetError());
        }
        return {};
    }

    ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
        step("create window");
        dk64nx_window_set_size(nx::screen_width(), nx::screen_height());
        dk64nx_window = SDL_CreateWindow("DK64: Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                         nx::screen_width(), nx::screen_height(), SDL_WINDOW_OPENGL);
        if (dk64nx_window == nullptr) {
            fprintf(stderr, "[DK64NX] SDL_CreateWindow failed: %s\n", SDL_GetError());
        }
        step("window created");
        return dk64nx_window;
    }

    void update_gfx(void*) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                ultramodern::quit();
            }
            dk64nx_input_handle_event(event);
        }
        dk64nx_perf_update();
    }

    RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
        if (task->t.type == M_AUDTASK) {
            return n_aspMain;
        }
        fprintf(stderr, "[DK64NX] Unknown RSP task type %" PRIu32 "\n", task->t.type);
        return nullptr;
    }
}

// Audio path copied from src/main/main.cpp (SDL queue + resampler), volume fixed at 100%.
namespace {
    SDL_AudioCVT audio_convert;
    SDL_AudioDeviceID audio_device = 0;
    uint32_t sample_rate = 48000;
    uint32_t output_sample_rate = 48000;
    constexpr uint32_t input_channels = 2;
    uint32_t output_channels = 2;
    // Frames duplicated across chunks so the resampler interpolates over the seam.
    constexpr uint32_t duplicated_input_frames = 4;
    uint32_t discarded_output_frames;
    constexpr uint32_t bytes_per_frame = input_channels * sizeof(float);
    // Queue depth past which whole chunks are dropped (see queue_samples).
    constexpr uint64_t MAX_QUEUED_AUDIO_US = 250000;
    constexpr Uint16 AUDIO_BUFFER_FRAMES = 1024;

    void queue_samples(int16_t* audio_data, size_t sample_count) {
        static std::vector<float> swap_buffer;
        static std::array<float, duplicated_input_frames * input_channels> duplicated_sample_buffer;

        size_t resampled_sample_count = sample_count + duplicated_input_frames * input_channels;
        size_t max_sample_count = std::max(resampled_sample_count, resampled_sample_count * audio_convert.len_mult);
        if (max_sample_count > swap_buffer.size()) {
            swap_buffer.resize(max_sample_count);
        }
        for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
            swap_buffer[i] = duplicated_sample_buffer[i];
        }
        // NX Options' Master Volume scales everything the game mixed (music and effects have their own settings).
        const float gain = (0.5f / 32768.0f) * static_cast<float>(nx::get(nx::Setting::MasterVolume)) / 100.0f;
        // Swap the channels back: the rdram address xor swaps adjacent 16-bit samples.
        for (size_t i = 0; i < sample_count; i += input_channels) {
            swap_buffer[i + 0 + duplicated_input_frames * input_channels] = audio_data[i + 1] * gain;
            swap_buffer[i + 1 + duplicated_input_frames * input_channels] = audio_data[i + 0] * gain;
        }
        for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
            duplicated_sample_buffer[i] = swap_buffer[i + sample_count];
        }

        audio_convert.buf = reinterpret_cast<Uint8*>(swap_buffer.data());
        audio_convert.len = (sample_count + duplicated_input_frames * input_channels) * sizeof(swap_buffer[0]);
        if (SDL_ConvertAudio(&audio_convert) < 0) {
            throw std::runtime_error(std::string("SDL_ConvertAudio failed: ") + SDL_GetError());
        }

        // The SDL queue holds converted frames at the output rate; upstream divides by the game's rate
        // (22050 Hz for DK64), overestimating latency 2.2x and thinning audio (high pitch on Switch).
        uint64_t queued_us = uint64_t(SDL_GetQueuedAudioSize(audio_device)) / bytes_per_frame * 1000000 / output_sample_rate;
        uint32_t num_bytes_to_queue = audio_convert.len_cvt - output_channels * discarded_output_frames * sizeof(swap_buffer[0]);
        float* samples_to_queue = swap_buffer.data() + output_channels * discarded_output_frames / 2;

        // Upstream thins each chunk by 2^(queued/100 ms), which shifts the pitch up; on Switch the
        // queue sits around 110 ms on its own, so that guard fired constantly. Drop whole chunks
        // only past a real runaway instead: a short gap, never a pitch change.
        if (queued_us > MAX_QUEUED_AUDIO_US) {
            return;
        }
        SDL_QueueAudio(audio_device, samples_to_queue, num_bytes_to_queue);
    }

    size_t get_frames_remaining() {
        constexpr float buffer_offset_frames = 1.0f;
        uint64_t buffered_byte_count = SDL_GetQueuedAudioSize(audio_device);
        buffered_byte_count = buffered_byte_count * 2 * sample_rate / output_sample_rate / output_channels;
        uint32_t frames_per_vi = sample_rate / 60;
        if (buffered_byte_count > (buffer_offset_frames * bytes_per_frame * frames_per_vi)) {
            buffered_byte_count -= (buffer_offset_frames * bytes_per_frame * frames_per_vi);
        }
        else {
            buffered_byte_count = 0;
        }
        return static_cast<uint32_t>(buffered_byte_count / bytes_per_frame);
    }

    void update_audio_converter() {
        if (SDL_BuildAudioCVT(&audio_convert, AUDIO_F32, input_channels, sample_rate, AUDIO_F32, output_channels, output_sample_rate) < 0) {
            throw std::runtime_error(std::string("SDL_BuildAudioCVT failed: ") + SDL_GetError());
        }
        discarded_output_frames = duplicated_input_frames * output_sample_rate / sample_rate;
    }

    void set_frequency(uint32_t freq) {
        printf("[DK64NX] audio frequency set to %u Hz\n", freq);
        sample_rate = freq;
        update_audio_converter();
    }

    bool reset_audio(uint32_t output_freq) {
        SDL_AudioSpec spec_desired{};
        spec_desired.freq = static_cast<int>(output_freq);
        spec_desired.format = AUDIO_F32;
        spec_desired.channels = static_cast<Uint8>(output_channels);
        // Upstream asks for 256-frame buffers to keep PC latency low. SDL's Switch backend releases one
        // buffer per ~10 ms audio service tick, which capped playback at 25.6k of 48k frames/s; 1024
        // frames (21 ms) leaves ample headroom.
        spec_desired.samples = AUDIO_BUFFER_FRAMES;
        audio_device = SDL_OpenAudioDevice(nullptr, false, &spec_desired, nullptr, 0);
        if (audio_device == 0) {
            fprintf(stderr, "[DK64NX] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_PauseAudioDevice(audio_device, 0);
        output_sample_rate = output_freq;
        update_audio_converter();
        return true;
    }
}

#define REGISTER_FUNC(name) recomp::overlays::register_base_export(#name, name)

// The .nro's path, for Restart Game (nx_menu.cpp).
const char* dk64nx_program_path = nullptr;

int main(int argc, char** argv) {
#ifdef DK64NX_REMOTE
    dk64nx_trace("main: start");
#endif
    if (argc > 0) dk64nx_program_path = argv[0];
    std::set_terminate(log_terminate);
    init_logging();
    printf("[DK64NX] DK64: Recompiled %s for Switch, applet type %d (%llu ms)\n", VERSION_STRING.c_str(),
           static_cast<int>(appletGetAppletType()), dk64nx_ms_since_boot());
    nx::load();
    nx::cheats::load();
    dk64nx_perf_start();
#ifdef DK64NX_REMOTE
    dk64nx_remote_start();
#endif

    recomp::Version project_version{};
    recomp::Version::from_string(VERSION_STRING, project_version);

    step("audio init");
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!reset_audio(48000)) {
        return EXIT_FAILURE;
    }

    step("register game");
    recomp::register_config_path(DATA_DIR);

    recomp::GameEntry game{
        .rom_hash = 0x4d876060f09b3fc5ULL,
        .internal_name = "DONKEY KONG 64",
        .display_name = "Donkey Kong 64",
        .game_id = GAME_ID,
        .mod_game_id = MOD_GAME_ID,
        .discovery_url = "",
        // Eep16k instead of Eep4k to have room for extra save file data (same as PC).
        .save_type = recomp::SaveType::Eep16k,
        .thumbnail_bytes = std::span<const char>{},
        .is_enabled = false,
        .decompression_routine = dk64::decompress_dk,
        .has_compressed_code = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
        .on_init_callback = dk64::dk_on_init,
    };
    recomp::register_game(game);

    REGISTER_FUNC(recomp_get_window_resolution);
    REGISTER_FUNC(recomp_get_target_aspect_ratio);
    REGISTER_FUNC(recomp_get_target_framerate);
    REGISTER_FUNC(recomp_get_right_analog_inputs);
    REGISTER_FUNC(recomp_get_bgm_volume);
    REGISTER_FUNC(recomp_get_sfx_volume);
    REGISTER_FUNC(recomp_get_draw_distance);
    REGISTER_FUNC(recomp_get_story_skip);
    REGISTER_FUNC(recomp_get_camera_type);
    REGISTER_FUNC(recomp_get_lightning_frequency);
    REGISTER_FUNC(recomp_get_cutscene_bordering);
    REGISTER_FUNC(recomp_get_mp_enabled);
    REGISTER_FUNC(recomp_get_ui_bounds);
    REGISTER_FUNC(recomp_get_ui_pillar);
    REGISTER_FUNC(recomp_nx_get_frame_vis);
    REGISTER_FUNC(recomp_nx_next_frame_lag);
    REGISTER_FUNC(recomp_nx_take_warp);
    REGISTER_FUNC(recomp_get_gyro_deltas);
    REGISTER_FUNC(recomp_get_mouse_deltas);
    REGISTER_FUNC(recomp_get_inverted_axes);
    REGISTER_FUNC(recomp_get_analog_inverted_axes);
    REGISTER_FUNC(recomp_get_swimming_inverted_axes);
    REGISTER_FUNC(recomp_get_first_person_inverted_axes);
    REGISTER_FUNC(recomp_get_gyro_inverted_axes);
    REGISTER_FUNC(recomp_get_mouse_inverted_axes);
    recomputil::register_data_api_exports();

    dk64::register_bk_overlays();
    dk64::register_bk_patches();
    // Extensions for two object types: Props and ActorMarkers.
    recomputil::init_extended_object_data(2);
    dk64::init_config();
    set_switch_graphics_config();
    step("rom check");

    if (!ensure_rom()) {
        return EXIT_FAILURE;
    }

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };
    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = dk64nx_create_render_context,
    };
    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };
    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };
    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = recompinput::poll_inputs,
        .get_input = dk64nx_get_input,
        .set_rumble = recompinput::set_rumble,
        .get_connected_device_info = dk64nx_get_connected_device_info,
    };
    ultramodern::events::callbacks_t events_callbacks{
        .vi_callback = recompinput::update_rumble,
        .gfx_init_callback = nullptr,
    };
    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = recompui::message_box,
    };
    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = nullptr,
    };

    // Boot the game directly, as `DK64Recompiled --game dk64` does on PC.
    char program_name[] = "dk64recomp";
    char game_flag[] = "--game";
    char* boot_argv[] = {program_name, game_flag, MOD_GAME_ID, nullptr};
    (void)argc;
    (void)argv;

    recomp::Configuration cfg{
        .argc = 3,
        .argv = boot_argv,
        .project_version = project_version,
        .window_handle = {},
        .rsp_callbacks = rsp_callbacks,
        .renderer_callbacks = renderer_callbacks,
        .audio_callbacks = audio_callbacks,
        .input_callbacks = input_callbacks,
        .gfx_callbacks = gfx_callbacks,
        .events_callbacks = events_callbacks,
        .error_handling_callbacks = error_handling_callbacks,
        .threads_callbacks = threads_callbacks,
        .message_queue_control = {
            .requeue_timer = false,
        },
    };
    step("recomp::start");
    recomp::start(cfg);
    step("recomp::start returned");
#ifdef DK64NX_REMOTE
    dk64nx_remote_stop();
#endif
    // The homebrew loader reuses this process: threads left running would execute unmapped code, and their stacks
    // (like the mods' code memory) would keep pages of the heap the next .nro gets.
    const int threads_left = ultramodern::stop_game_threads();
    printf("[DK64NX] game threads stopped, %d left\n", threads_left);
    dk64nx_jit_close();
    dk64nx_perf_stop();
    step("SDL_Quit");
    SDL_Quit();
    socketExit();
    step("back to the loader");
    // Everything that has to stop is stopped above; the loader reclaims the rest. _exit skips the global C++
    // destructors, which may wait on threads that are gone, and still runs libnx's cleanup before returning to the
    // loader (which then starts the .nro again after Restart Game).
    fflush(nullptr);
#ifdef DK64NX_REMOTE
    dk64nx_trace("main: _exit");
#endif
    if (threads_left > 0) {
        // Going back to the loader would crash: end the whole process instead (no restart).
        svcExitProcess();
    }
    _exit(EXIT_SUCCESS);
}
