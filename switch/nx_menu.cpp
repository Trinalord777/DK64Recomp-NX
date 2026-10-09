// "NX Options": the in-game settings menu, opened and closed with - and + together. Drawn with Dear ImGui
// over the frame about to be presented, from nx_VidExt_GL_SwapBuffers on the graphics thread. While it is open
// it owns the pad: the game sees a neutral pad (input_switch.cpp) until every button is released after closing.
// Nothing touches GL before the menu or the FPS counter is first shown.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <glad/glad.h>
#include <SDL.h>
#include <switch.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "nx_cheats.h"
#include "nx_pad.h"
#include "nx_settings.h"

namespace {
    using nx::Setting;

    // Rows: the settings, then the cheats of cheats.cht, then Quit Game.
    constexpr int SETTING_COUNT = static_cast<int>(Setting::Count);
    constexpr uint32_t TOGGLE = NX_MINUS | NX_PLUS;
    constexpr uint32_t DIRECTIONS = NX_UP | NX_DOWN | NX_LEFT | NX_RIGHT;
    constexpr uint64_t REPEAT_DELAY_MS = 400;
    constexpr uint64_t REPEAT_INTERVAL_MS = 100;
    constexpr float STICK_PRESS = 0.6f;
    constexpr float STICK_RELEASE = 0.4f;
    constexpr float FONT_SIZE = 24.0f;
    constexpr ImVec2 PANEL_SIZE{780.0f, 640.0f};
    constexpr float ROW_PADDING = 6.0f;
    constexpr ImU32 ACCENT = IM_COL32(255, 145, 30, 255);
    constexpr ImU32 ACCENT_FILL = IM_COL32(255, 145, 30, 48);
    constexpr ImU32 TEXT = IM_COL32(235, 235, 235, 255);
    constexpr ImU32 TEXT_DIM = IM_COL32(150, 150, 150, 255);

    std::atomic_bool owns_pad{false};
    bool is_open = false;
    // Closed, but the pad stays away from the game until every button is up, so B, - or + do not leak into it.
    bool release_pending = false;
    bool changed = false;
    bool quit_armed = false;
    bool scroll_to_selection = false;
    int selected = 0;
    uint32_t previous_buttons = 0;
    uint32_t stick_direction = 0;
    uint64_t repeat_at = 0;

    enum class ImGuiState { NotStarted, Ready, Failed };
    ImGuiState imgui_state = ImGuiState::NotStarted;
    uint64_t last_frame_ms = 0;

    int fps = 0;
    int frames_counted = 0;
    uint64_t fps_window_start_ms = 0;

    // The left stick acts as the D-pad along its dominant axis only (a diagonal would move and change a value at
    // once), pressed past STICK_PRESS and released below STICK_RELEASE.
    int row_count() { return SETTING_COUNT + nx::cheats::count() + 1; }
    bool is_quit(int row) { return row == row_count() - 1; }
    bool is_cheat(int row) { return row >= SETTING_COUNT && !is_quit(row); }
    int cheat_index(int row) { return row - SETTING_COUNT; }

    void save_changes() {
        if (changed) {
            nx::save();
            changed = false;
        }
        nx::cheats::save();
    }

    uint32_t menu_buttons(const NxPad& pad) {
        const float x = std::fabs(pad.lx);
        const float y = std::fabs(pad.ly);
        const float magnitude = std::max(x, y);
        if (magnitude < STICK_RELEASE) {
            stick_direction = 0;
        }
        else if (magnitude > STICK_PRESS || stick_direction != 0) {
            stick_direction = y >= x ? (pad.ly > 0 ? NX_UP : NX_DOWN) : (pad.lx > 0 ? NX_RIGHT : NX_LEFT);
        }
        return pad.buttons | stick_direction;
    }

    void open_menu() {
        is_open = true;
        owns_pad = true;
        release_pending = false;
        changed = false;
        quit_armed = false;
        scroll_to_selection = true;
    }

    void close_menu() {
        is_open = false;
        release_pending = true;
        quit_armed = false;
        save_changes();
    }

    void move_selection(int direction) {
        selected = (selected + direction + row_count()) % row_count();
        quit_armed = false;
        scroll_to_selection = true;
    }

    // A cycles through the values; left/right stop at the ends of a number range.
    void change_selected(int direction, bool wrap) {
        if (is_quit(selected)) {
            if (!wrap) return;
            if (quit_armed) {
                save_changes();
                SDL_Event quit{};
                quit.type = SDL_QUIT;
                SDL_PushEvent(&quit);
            }
            quit_armed = true;
            return;
        }
        if (is_cheat(selected)) {
            nx::cheats::toggle(cheat_index(selected));
            return;
        }
        const Setting setting = static_cast<Setting>(selected);
        const nx::SettingInfo& info = nx::info(setting);
        const int value = nx::get(setting);
        if (info.choices.size() > 0) {
            const int count = static_cast<int>(info.choices.size());
            nx::set(setting, (value + direction + count) % count);
        }
        else {
            int next = value + direction * info.step;
            if (wrap && next > info.max) next = info.min;
            nx::set(setting, next);
        }
        changed = true;
    }

    void update_input(uint64_t now) {
        NxPad pad;
        dk64nx_read_pad(&pad);
        const uint32_t held = menu_buttons(pad);
        const uint32_t pressed = held & ~previous_buttons;
        previous_buttons = held;

        if ((held & TOGGLE) == TOGGLE && (pressed & TOGGLE) != 0 && imgui_state != ImGuiState::Failed) {
            is_open ? close_menu() : open_menu();
            repeat_at = now + REPEAT_DELAY_MS;
            return;
        }
        if (!is_open) {
            if (release_pending && held == 0) {
                release_pending = false;
                owns_pad = false;
            }
            return;
        }

        uint32_t actions = pressed;
        if ((pressed & DIRECTIONS) != 0) {
            repeat_at = now + REPEAT_DELAY_MS;
        }
        else if ((held & DIRECTIONS) != 0 && now >= repeat_at) {
            actions |= held & DIRECTIONS;
            repeat_at = now + REPEAT_INTERVAL_MS;
        }
        if (actions & NX_B) {
            close_menu();
            return;
        }
        if (actions & NX_UP) move_selection(-1);
        if (actions & NX_DOWN) move_selection(1);
        if (actions & NX_LEFT) change_selected(-1, false);
        if (actions & NX_RIGHT) change_selected(1, false);
        if (actions & NX_A) change_selected(1, true);
    }

    void count_frame(uint64_t now) {
        frames_counted++;
        if (now - fps_window_start_ms >= 1000) {
            fps = static_cast<int>(frames_counted * 1000 / (now - fps_window_start_ms));
            frames_counted = 0;
            fps_window_start_ms = now;
        }
    }

    void apply_style() {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 14.0f;
        style.WindowPadding = {28.0f, 22.0f};
        style.WindowBorderSize = 0.0f;
        style.ItemSpacing = {8.0f, 4.0f};
        style.ScrollbarSize = 6.0f;
        style.Colors[ImGuiCol_WindowBg] = {0.07f, 0.07f, 0.08f, 0.94f};
        style.Colors[ImGuiCol_ChildBg] = {0.0f, 0.0f, 0.0f, 0.0f};
        style.Colors[ImGuiCol_Separator] = {1.0f, 1.0f, 1.0f, 0.12f};
        style.Colors[ImGuiCol_ScrollbarBg] = {0.0f, 0.0f, 0.0f, 0.0f};
        style.Colors[ImGuiCol_ScrollbarGrab] = {1.0f, 1.0f, 1.0f, 0.18f};
        style.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(TEXT);
    }

    bool start_imgui() {
        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) == 0) {
            std::fprintf(stderr, "[DK64NX] menu: GL functions not found\n");
            return false;
        }
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        // The console's own font; pl stays initialized because the font lives in its shared memory.
        PlFontData font{};
        if (R_SUCCEEDED(plInitialize(PlServiceType_User)) &&
            R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) {
            ImFontConfig config;
            config.FontDataOwnedByAtlas = false;
            io.Fonts->AddFontFromMemoryTTF(font.address, static_cast<int>(font.size), FONT_SIZE, &config);
        }
        else {
            ImFontConfig config;
            config.SizePixels = FONT_SIZE;
            io.Fonts->AddFontDefault(&config);
        }
        apply_style();
        // GLideN64 creates a 3.3 core profile context.
        return ImGui_ImplOpenGL3_Init("#version 330 core");
    }

    std::string value_text(int row) {
        if (is_quit(row)) {
            return quit_armed ? "Press A again" : "";
        }
        if (is_cheat(row)) {
            const int cheat = cheat_index(row);
            return !nx::cheats::supported(cheat) ? "Unsupported" : nx::cheats::enabled(cheat) ? "On" : "Off";
        }
        const Setting setting = static_cast<Setting>(row);
        const nx::SettingInfo& info = nx::info(setting);
        const int value = nx::get(setting);
        std::string text = info.choices.size() > 0 ? info.choices.begin()[value]
                                                    : std::to_string(value) + info.suffix;
        if (info.needs_restart && value != nx::at_startup(setting)) {
            text += " *";
        }
        return text;
    }

    const char* row_label(int row) {
        if (is_quit(row)) return "Quit Game";
        if (is_cheat(row)) return nx::cheats::name(cheat_index(row));
        return nx::info(static_cast<Setting>(row)).label;
    }

    const char* row_section(int row) {
        if (is_quit(row)) return "System";
        if (is_cheat(row)) return "Cheats";
        return nx::info(static_cast<Setting>(row)).section;
    }

    const char* row_description(int row) {
        if (is_quit(row)) {
            return "Closes the game and returns to the homebrew menu. Progress since your last save is lost.";
        }
        if (is_cheat(row)) {
            return nx::cheats::supported(cheat_index(row))
                ? "GameShark code from cheats.cht, written into the game's memory every frame while On."
                : "This code rewrites the game's MIPS instructions, which the recompiled game no longer runs.";
        }
        return nx::info(static_cast<Setting>(row)).description;
    }

    void draw_row(int row) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetTextLineHeight() + 2 * ROW_PADDING;
        ImGui::Dummy({width, height});
        const bool is_selected = row == selected;
        if (is_selected) {
            if (scroll_to_selection) {
                ImGui::SetScrollHereY(0.5f);
                scroll_to_selection = false;
            }
            draw->AddRectFilled(start, {start.x + width, start.y + height}, ACCENT_FILL, 6.0f);
            draw->AddRectFilled(start, {start.x + 4.0f, start.y + height}, ACCENT, 2.0f);
        }
        draw->AddText({start.x + 18.0f, start.y + ROW_PADDING}, TEXT, row_label(row));
        std::string value = value_text(row);
        if (is_selected && !is_quit(row)) {
            value = "<  " + value + "  >";
        }
        const float value_width = ImGui::CalcTextSize(value.c_str()).x;
        draw->AddText({start.x + width - 18.0f - value_width, start.y + ROW_PADDING}, is_selected ? ACCENT : TEXT_DIM,
                      value.c_str());
    }

    void draw_menu(float screen_width, float screen_height) {
        ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, {screen_width, screen_height},
                                                      IM_COL32(0, 0, 0, 150));
        ImGui::SetNextWindowPos({(screen_width - PANEL_SIZE.x) / 2, (screen_height - PANEL_SIZE.y) / 2});
        ImGui::SetNextWindowSize(PANEL_SIZE);
        ImGui::Begin("NX Options", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);

        ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
        ImGui::TextUnformatted("NX OPTIONS");
        ImGui::PopStyleColor();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("DK64: Recompiled").x);
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_DIM);
        ImGui::TextUnformatted("DK64: Recompiled");
        ImGui::PopStyleColor();
        ImGui::Separator();

        const float footer_height = 5 * ImGui::GetTextLineHeightWithSpacing() + 16.0f;
        ImGui::BeginChild("rows", {0.0f, -footer_height});
        const char* section = nullptr;
        for (int row = 0; row < row_count(); row++) {
            if (section == nullptr || std::strcmp(section, row_section(row)) != 0) {
                section = row_section(row);
                ImGui::Dummy({0.0f, 6.0f});
                ImGui::PushStyleColor(ImGuiCol_Text, TEXT_DIM);
                ImGui::TextUnformatted(section);
                ImGui::PopStyleColor();
            }
            draw_row(row);
        }
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(row_description(selected));
        ImGui::PopTextWrapPos();
        if (selected < SETTING_COUNT && nx::info(static_cast<Setting>(selected)).needs_restart) {
            ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
            ImGui::TextUnformatted("* Applies the next time the game starts.");
            ImGui::PopStyleColor();
        }
        const char* hint = "Up/Down  Select     Left/Right/A  Change     B  Close";
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - ImGui::GetTextLineHeight());
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT_DIM);
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
        ImGui::End();
    }

    void draw_fps() {
        char text[16];
        std::snprintf(text, sizeof(text), "%d FPS", fps);
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddRectFilled({12.0f, 12.0f}, {28.0f + size.x, 20.0f + size.y}, IM_COL32(0, 0, 0, 160), 6.0f);
        draw->AddText({20.0f, 16.0f}, TEXT, text);
    }

    // GLideN64 keeps its own GL state cache: everything the ImGui backend does not restore itself is put back
    // here, so the next emulated frame finds GL exactly as it left it.
    void render(int width, int height, uint64_t now) {
        GLint draw_framebuffer = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = {static_cast<float>(width), static_cast<float>(height)};
        io.DeltaTime = std::max(0.001f, (now - last_frame_ms) / 1000.0f);
        last_frame_ms = now;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        if (is_open) draw_menu(io.DisplaySize.x, io.DisplaySize.y);
        if (nx::get(Setting::ShowFps) != 0) draw_fps();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_framebuffer));
    }
}

bool dk64nx_menu_owns_pad() {
    return owns_pad.load();
}

// Graphics thread, right before the swap (declared inside core_switch.cpp's extern "C" block).
extern "C" void dk64nx_menu_on_swap(int width, int height) {
    const uint64_t now = SDL_GetTicks64();
    count_frame(now);
    update_input(now);
    nx::cheats::apply();
    if (!is_open && nx::get(Setting::ShowFps) == 0) return;
    if (imgui_state == ImGuiState::NotStarted) {
        imgui_state = start_imgui() ? ImGuiState::Ready : ImGuiState::Failed;
    }
    if (imgui_state == ImGuiState::Failed) {
        // A menu nobody can see must not keep the pad from the game.
        is_open = false;
        release_pending = false;
        owns_pad = false;
        return;
    }
    render(width, height, now);
}
