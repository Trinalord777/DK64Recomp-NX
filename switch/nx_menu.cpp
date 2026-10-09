// "NX Options": the in-game settings menu, opened and closed with - and + together. Drawn with Dear ImGui
// over the frame about to be presented, from nx_VidExt_GL_SwapBuffers on the graphics thread. While it is open
// it owns the pad: the game sees a neutral pad (input_switch.cpp) until every button is released after closing.
// Nothing touches GL before the menu or the FPS counter is first shown.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <glad/glad.h>
#include <SDL.h>
#include <switch.h>

#include "imgui.h"
#include "imgui_internal.h"  // RenderTextEllipsis
#include "librecomp/mods.hpp"
#include "imgui_impl_opengl3.h"
#include "nx_cheats.h"
#include "nx_pad.h"
#include "nx_settings.h"

namespace {
    using nx::Setting;

    // Rows: the settings, the mods, the cheats of cheats.cht, then Quit Game.
    constexpr int SETTING_COUNT = static_cast<int>(Setting::Count);
    constexpr const char* MOD_GAME_ID = "dk64";

    // Read when the menu opens; mods load when the game starts, so a change needs a restart.
    std::vector<recomp::mods::ModDetails> mods;
    // One row per mod, then one per option of its config schema (Enum, Number and Bool options).
    struct ModRow {
        size_t mod;
        int option;  // -1: the mod itself
    };
    std::vector<ModRow> mod_rows;
    std::vector<const recomp::config::ConfigSchema*> mod_schemas;
    std::string mod_label;
    std::map<std::string, bool> mod_enabled_at_start;
    // Once the game runs, librecomp ignores enable_mod for mods that are not runtime toggleable (all of DK64's so
    // far), so the menu keeps its own choices and writes them to mods.json for the next start.
    std::map<std::string, bool> mod_chosen;
    bool mods_dirty = false;
    std::string mod_description;
    constexpr uint32_t TOGGLE = NX_MINUS | NX_PLUS;
    constexpr uint32_t DIRECTIONS = NX_UP | NX_DOWN | NX_LEFT | NX_RIGHT;
    constexpr uint64_t REPEAT_DELAY_MS = 400;
    constexpr uint64_t REPEAT_INTERVAL_MS = 100;
    constexpr float STICK_PRESS = 0.6f;
    constexpr float STICK_RELEASE = 0.4f;
    constexpr float FONT_SIZE = 27.0f;
    // Share of the 1280x720 frame the panel covers (the console scales the frame to the TV).
    constexpr ImVec2 PANEL_SHARE{0.82f, 0.92f};
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
    int mod_count() { return static_cast<int>(mod_rows.size()); }
    int row_count() { return SETTING_COUNT + mod_count() + nx::cheats::count() + 1; }
    bool is_quit(int row) { return row == row_count() - 1; }
    bool is_mod(int row) { return row >= SETTING_COUNT && row < SETTING_COUNT + mod_count(); }
    bool is_cheat(int row) { return row >= SETTING_COUNT + mod_count() && !is_quit(row); }
    int mod_index(int row) { return row - SETTING_COUNT; }
    int cheat_index(int row) { return row - SETTING_COUNT - mod_count(); }

    void refresh_mods() {
        using recomp::config::ConfigOptionType;
        mods = recomp::mods::get_all_mod_details(MOD_GAME_ID);
        mod_rows.clear();
        mod_schemas.clear();
        for (size_t mod = 0; mod < mods.size(); mod++) {
            const std::string& id = mods[mod].mod_id;
            mod_enabled_at_start.try_emplace(id, recomp::mods::is_mod_enabled(id) || recomp::mods::is_mod_auto_enabled(id));
            mod_chosen.try_emplace(id, recomp::mods::is_mod_enabled(id));
            const recomp::config::ConfigSchema& schema = recomp::mods::get_mod_config_schema(mods[mod].mod_id);
            mod_schemas.push_back(&schema);
            mod_rows.push_back({mod, -1});
            for (size_t option = 0; option < schema.options.size(); option++) {
                const ConfigOptionType type = schema.options[option].type;
                if (!schema.options[option].hidden &&
                    (type == ConfigOptionType::Enum || type == ConfigOptionType::Number || type == ConfigOptionType::Bool)) {
                    mod_rows.push_back({mod, static_cast<int>(option)});
                }
            }
        }
    }

    // Turned on by librecomp at the next start because a chosen mod depends on it.
    bool mod_needed(const std::string& id) {
        for (const recomp::mods::ModDetails& mod : mods) {
            if (!mod_chosen[mod.mod_id]) continue;
            for (const recomp::mods::Dependency& dependency : mod.dependencies) {
                if (!dependency.optional && dependency.mod_id == id) return true;
            }
        }
        return false;
    }

    // Keeps mod_order and latest_game_mode, which librecomp also stores there.
    void write_mods_json() {
        const std::filesystem::path path = recomp::mods::get_mods_directory().parent_path() / "mods.json";
        nlohmann::json json = nlohmann::json::object();
        if (std::ifstream input{path}; input.good()) {
            json = nlohmann::json::parse(input, nullptr, false);
            if (json.is_discarded() || !json.is_object()) json = nlohmann::json::object();
        }
        std::vector<std::string> enabled;
        for (const recomp::mods::ModDetails& mod : mods) {
            if (mod_chosen[mod.mod_id]) enabled.push_back(mod.mod_id);
        }
        json["enabled_mods"] = enabled;
        if (!json.contains("latest_game_mode")) json["latest_game_mode"] = "";
        std::ofstream output{path};
        output << json.dump(4);
        if (!output.good()) std::fprintf(stderr, "[DK64NX] cannot write %s\n", path.string().c_str());
    }

    const recomp::config::ConfigOption& mod_option(const ModRow& row) { return mod_schemas[row.mod]->options[row.option]; }

    std::string mod_option_text(const ModRow& row) {
        const recomp::config::ConfigOption& option = mod_option(row);
        recomp::config::Config* config = recomp::mods::get_mod_config(mods[row.mod].mod_id);
        if (config == nullptr) return "";
        const recomp::config::ConfigValueVariant value = config->get_option_value(option.id);
        if (const auto* number = std::get_if<uint32_t>(&value)) {
            const auto& choices = std::get<recomp::config::ConfigOptionEnum>(option.variant);
            const auto found = choices.find_option_from_value(*number);
            return found == choices.options.end() ? std::to_string(*number) : found->name;
        }
        if (const auto* flag = std::get_if<bool>(&value)) return *flag ? "On" : "Off";
        if (const auto* real = std::get_if<double>(&value)) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.*f", std::get<recomp::config::ConfigOptionNumber>(option.variant).precision, *real);
            return text;
        }
        return "";
    }

    // Saved by librecomp in mod_config/; the mod reads it through recomp_get_config_*, so most apply at once.
    void change_mod_option(const ModRow& row, int direction) {
        const recomp::config::ConfigOption& option = mod_option(row);
        const std::string& mod_id = mods[row.mod].mod_id;
        recomp::config::Config* config = recomp::mods::get_mod_config(mod_id);
        if (config == nullptr) return;
        const recomp::config::ConfigValueVariant value = config->get_option_value(option.id);
        if (const auto* number = std::get_if<uint32_t>(&value)) {
            const auto& choices = std::get<recomp::config::ConfigOptionEnum>(option.variant).options;
            size_t index = 0;
            while (index < choices.size() && choices[index].value != *number) index++;
            const size_t next = (index + choices.size() + direction) % choices.size();
            recomp::mods::set_mod_config_value(mod_id, option.id, choices[next].value);
        }
        else if (const auto* flag = std::get_if<bool>(&value)) {
            recomp::mods::set_mod_config_value(mod_id, option.id, !*flag);
        }
        else if (const auto* real = std::get_if<double>(&value)) {
            const auto& range = std::get<recomp::config::ConfigOptionNumber>(option.variant);
            recomp::mods::set_mod_config_value(mod_id, option.id, std::clamp(*real + direction * range.step, range.min, range.max));
        }
    }

    void save_changes() {
        if (changed) {
            nx::save();
            changed = false;
        }
        nx::cheats::save();
        if (mods_dirty) {
            write_mods_json();
            mods_dirty = false;
        }
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
        refresh_mods();
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

    const char* row_section(int row);

    // L/R: first row of the previous or next section (Mods and Cheats are long lists).
    void jump_section(int direction) {
        const int count = row_count();
        int row = selected;
        if (direction > 0) {
            while (row < count - 1 && std::strcmp(row_section(row), row_section(selected)) == 0) row++;
        }
        else {
            if (row > 0 && std::strcmp(row_section(row - 1), row_section(row)) != 0) row--;  // already at a start
            while (row > 0 && std::strcmp(row_section(row - 1), row_section(row)) == 0) row--;
        }
        selected = row;
        quit_armed = false;
        scroll_to_selection = true;
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
        if (is_mod(selected)) {
            const ModRow& row = mod_rows[mod_index(selected)];
            if (row.option >= 0) {
                change_mod_option(row, direction);
                return;
            }
            const std::string& id = mods[row.mod].mod_id;
            mod_chosen[id] = !mod_chosen[id];
            mods_dirty = true;
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
        if (pressed & NX_L) jump_section(-1);
        if (pressed & NX_R) jump_section(1);
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
        if (is_mod(row)) {
            const ModRow& mod_row = mod_rows[mod_index(row)];
            if (mod_row.option >= 0) return mod_option_text(mod_row);
            const std::string& id = mods[mod_row.mod].mod_id;
            const bool needed = !mod_chosen[id] && mod_needed(id);
            std::string text = mod_chosen[id] ? "On" : needed ? "On (needed)" : "Off";
            if ((mod_chosen[id] || needed) != mod_enabled_at_start[id]) text += " *";
            return text;
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
        if (is_mod(row)) {
            const ModRow& mod_row = mod_rows[mod_index(row)];
            if (mod_row.option < 0) return mods[mod_row.mod].display_name.c_str();
            mod_label = "    " + mod_option(mod_row).name;
            return mod_label.c_str();
        }
        return nx::info(static_cast<Setting>(row)).label;
    }

    const char* row_section(int row) {
        if (is_quit(row)) return "System";
        if (is_cheat(row)) return "Cheats";
        if (is_mod(row)) return "Mods";
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
        if (is_mod(row) && mod_rows[mod_index(row)].option >= 0) {
            return mod_option(mod_rows[mod_index(row)]).description.c_str();
        }
        if (is_mod(row)) {
            const recomp::mods::ModDetails& mod = mods[mod_rows[mod_index(row)].mod];
            mod_description = mod.short_description.empty() ? mod.description : mod.short_description;
            mod_description += " (version " + mod.version.to_string() + ")";
            return mod_description.c_str();
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
        std::string value = value_text(row);
        if (is_selected && !is_quit(row)) {
            value = "<  " + value + "  >";
        }
        const float value_width = ImGui::CalcTextSize(value.c_str()).x;
        const float value_x = start.x + width - 18.0f - value_width;
        // Long labels (cheats) end with an ellipsis before the value instead of running into it.
        const ImVec2 label_min{start.x + 18.0f, start.y + ROW_PADDING};
        const ImVec2 label_max{value_x - 16.0f, start.y + height};
        ImGui::PushStyleColor(ImGuiCol_Text, TEXT);
        ImGui::RenderTextEllipsis(draw, label_min, label_max, label_max.x, label_max.x, row_label(row), nullptr, nullptr);
        ImGui::PopStyleColor();
        draw->AddText({value_x, start.y + ROW_PADDING}, is_selected ? ACCENT : TEXT_DIM, value.c_str());
    }

    void draw_menu(float screen_width, float screen_height) {
        ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, {screen_width, screen_height},
                                                      IM_COL32(0, 0, 0, 150));
        const ImVec2 panel{screen_width * PANEL_SHARE.x, screen_height * PANEL_SHARE.y};
        ImGui::SetNextWindowPos({(screen_width - panel.x) / 2, (screen_height - panel.y) / 2});
        ImGui::SetNextWindowSize(panel);
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
        if ((is_mod(selected) && mod_rows[mod_index(selected)].option < 0) || (selected < SETTING_COUNT && nx::info(static_cast<Setting>(selected)).needs_restart)) {
            ImGui::PushStyleColor(ImGuiCol_Text, ACCENT);
            ImGui::TextUnformatted("* Applies the next time the game starts.");
            ImGui::PopStyleColor();
        }
        const char* hint = "Up/Down  Select    L/R  Section    Left/Right/A  Change    B  Close";
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
