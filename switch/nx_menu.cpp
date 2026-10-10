// "NX Options": the in-game settings menu, opened and closed with - and + together. Drawn with Dear ImGui
// over the frame about to be presented, from nx_VidExt_GL_SwapBuffers on the graphics thread. While it is open
// it owns the pad: the game sees a neutral pad (input_switch.cpp) until every button is released after closing.
// Nothing touches GL before the menu or the FPS counter is first shown.
// Layout after the HarbourMasters ports' menus: the tabs and the Help, Quit, Restart and Close buttons on top, the
// sections of the tab on the left, the rows of the chosen section on the right (widgets: nx_menu_widgets.cpp).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <glad/glad.h>
#include <SDL.h>
#include <switch.h>

#include "imgui.h"
#include "imgui_internal.h"  // RenderTextEllipsis
#include "librecomp/mods.hpp"
#include "imgui_impl_opengl3.h"
#include "nx_cheats.h"
#include "nx_dev_tools.h"
#include "nx_menu_widgets.h"
#include "nx_pad.h"
#include "nx_settings.h"

extern const char* dk64nx_program_path;
extern std::atomic<uint32_t> dk64nx_pass1_us;           // render_gliden64.cpp
extern std::atomic<uint32_t> dk64nx_pass2_us;
extern std::atomic<int> dk64nx_dropped_screen_updates;  // ultramodern events.cpp
extern std::atomic<bool> dk64nx_vsync_paced;              // vsync_switch.cpp

namespace {
    using nx::Setting;
    using nx::menu::Icon;
    using nx::menu::Theme;

    // Rows: the settings, the mods, the cheats of cheats.cht, Restart Game and Quit Game, then the dev tools. Each tab
    // shows some of them, in its own order (tab_rows), split into the sections of its sidebar; Restart and Quit are
    // the red buttons of the top bar.
    constexpr int SETTING_COUNT = static_cast<int>(Setting::Count);
    enum Tab { TAB_SETTINGS, TAB_ENHANCEMENTS, TAB_DEV_TOOLS, TAB_COUNT };
    constexpr const char* TAB_NAMES[TAB_COUNT] = {"Settings", "Enhancements", "Dev Tools"};
    // Settings of the Settings and Enhancements tabs, in display order; mods and cheats follow the Enhancements ones.
    constexpr Setting SETTINGS_TAB[] = {
        Setting::Resolution, Setting::AspectRatio, Setting::TextureLod, Setting::TextureFilter,
        Setting::ShowFps, Setting::PerformanceOverlay, Setting::InputViewer, Setting::GameplayTimer,
        Setting::FrameRate, Setting::FrameMode, Setting::ClockProfile,
        Setting::MasterVolume, Setting::MusicVolume, Setting::SoundVolume,
        Setting::FaceButtons,
        Setting::CameraType, Setting::CameraSensitivity, Setting::InvertCamera, Setting::InvertFirstPerson,
        Setting::InvertSwimming,
        Setting::MenuTheme, Setting::MenuOpacity, Setting::MenuSize,
    };
    constexpr Setting ENHANCEMENTS_TAB[] = {
        Setting::StorySkip, Setting::LightningFlashes, Setting::CutsceneBorders, Setting::DrawDistance,
    };
    // Cheats are listed by group, the text before the first " / " of their name: these first, in this order (the
    // Kongs as the game introduces them), then the others as cheats.cht lists them.
    constexpr const char* CHEAT_GROUP_ORDER[] = {"Infinite", "Donkey Kong", "Diddy", "Lanky", "Tiny", "Chunky"};
    constexpr const char* CHEAT_SEPARATOR = " / ";
    constexpr Icon ICONS[] = {Icon::Help, Icon::Quit, Icon::Restart, Icon::Close};
    constexpr int ICON_COUNT = static_cast<int>(std::size(ICONS));

    using nx::dev::Tool;
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
    std::map<std::string, bool> mod_enabled_at_start;
    // Once the game runs, librecomp ignores enable_mod for mods that are not runtime toggleable (all of DK64's so
    // far), so the menu keeps its own choices and writes them to mods.json for the next start.
    std::map<std::string, bool> mod_chosen;
    bool mods_dirty = false;
    std::string mod_description;
    std::vector<std::string> cheat_groups;  // per cheat, from its name
    constexpr uint32_t TOGGLE = NX_MINUS | NX_PLUS;
    constexpr uint32_t DIRECTIONS = NX_UP | NX_DOWN | NX_LEFT | NX_RIGHT;
    constexpr uint64_t REPEAT_DELAY_MS = 400;
    constexpr uint64_t REPEAT_INTERVAL_MS = 100;
    constexpr float STICK_PRESS = 0.6f;
    constexpr float STICK_RELEASE = 0.4f;
    // Menu Size: the font in pixels for Small, Medium and Large; the layout's sizes below are for Medium and scale.
    constexpr float FONT_SIZES[] = {23.0f, 27.0f, 32.0f};
    // Share of the 1280x720 frame the panel covers (the console scales the frame to the TV).
    constexpr ImVec2 PANEL_SHARE{0.92f, 0.90f};
    constexpr float BAR_HEIGHT = 44.0f;  // tabs and icon buttons
    constexpr float SIDEBAR_WIDTH = 210.0f;
    constexpr float SECTION_HEIGHT = 44.0f;
    constexpr float ROW_PADDING = 7.0f;
    // Widths of the widgets on the right of a row.
    constexpr float CHOICE_MIN_WIDTH = 240.0f;
    constexpr float SLIDER_WIDTH = 300.0f;
    constexpr float STEPPER_MIN_WIDTH = 220.0f;
    constexpr int FOOTER_LINES = 5;  // description (three lines at most), the restart note, the hints
    constexpr ImU32 OVERLAY_TEXT = IM_COL32(235, 235, 235, 255);

    // Where the pad acts: the sections, the rows of the chosen section, or the buttons of the top bar; a modal box
    // (an open choice list, a confirmation, the help) takes the pad until it closes.
    enum class Zone { Sidebar, Content, Toolbar };
    enum class Modal { None, Choices, Confirm, Help };
    enum class Widget { Text, Checkbox, Choice, Slider, Stepper, Button };

    std::atomic_bool owns_pad{false};
    bool is_open = false;
    // Closed, but the pad stays away from the game until every button is up, so B, - or + do not leak into it.
    bool release_pending = false;
    bool changed = false;
    bool scroll_to_selection = false;
    bool scroll_to_top = false;
    int selected = -1;  // the focused row (a row id) while the zone is Content
    int tab = TAB_SETTINGS;
    std::vector<int> tab_rows[TAB_COUNT];
    std::vector<const char*> tab_sections[TAB_COUNT];  // in order of first appearance among the tab's rows
    int tab_section[TAB_COUNT] = {};                   // the chosen section of each tab
    std::map<std::string, int> section_row;            // last focused row of each "tab/section"
    Zone zone = Zone::Sidebar;
    int icon = 0;
    Modal modal = Modal::None;
    bool confirm_restart = false;
    std::vector<const char*> choice_items;  // the open choice list of the selected row
    int choice_index = 0;
    ImVec2 choice_box_min{};  // where the selected row's choice box was drawn, to open its list under it
    ImVec2 choice_box_max{};
    uint32_t previous_buttons = 0;
    uint32_t stick_direction = 0;
    uint64_t repeat_at = 0;

    enum class ImGuiState { NotStarted, Ready, Failed };
    ImGuiState imgui_state = ImGuiState::NotStarted;
    uint64_t last_frame_ms = 0;

    int fps = 0;
    int frames_counted = 0;
    uint64_t fps_window_start_ms = 0;

    const Theme& look() { return nx::menu::theme(nx::get(Setting::MenuTheme)); }
    float font_size() { return FONT_SIZES[std::clamp(nx::get(Setting::MenuSize), 0, 2)]; }
    float px(float medium) { return medium * font_size() / FONT_SIZES[1]; }

    // The console's own font, kept to load it again at another Menu Size.
    PlFontData shared_font{};
    bool have_shared_font = false;
    float loaded_font_size = 0.0f;

    int mod_count() { return static_cast<int>(mod_rows.size()); }
    int system_row() { return SETTING_COUNT + mod_count() + nx::cheats::count(); }  // Restart Game, then Quit Game
    bool is_restart(int row) { return row == system_row(); }
    bool is_quit(int row) { return row == system_row() + 1; }
    bool is_dev_tool(int row) { return row >= system_row() + 2; }
    const Tool& dev_tool(int row) { return nx::dev::tool(row - system_row() - 2); }
    // The homebrew loader starts the .nro again on exit when told so (envSetNextLoad); not every loader can.
    bool can_restart() { return dk64nx_program_path != nullptr && envHasNextLoad(); }
    bool is_mod(int row) { return row >= SETTING_COUNT && row < SETTING_COUNT + mod_count(); }
    bool is_cheat(int row) { return row >= SETTING_COUNT + mod_count() && row < system_row(); }
    bool is_setting(int row) { return row >= 0 && row < SETTING_COUNT; }
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

    // A cheat's group is the text before the first " / " of its name ("Other" without one).
    std::vector<int> cheats_in_order() {
        cheat_groups.clear();
        for (int cheat = 0; cheat < nx::cheats::count(); cheat++) {
            const std::string name = nx::cheats::name(cheat);
            const size_t separator = name.find(CHEAT_SEPARATOR);
            cheat_groups.push_back(separator == std::string::npos ? "Other" : name.substr(0, separator));
        }
        // Sort key: the group's place in CHEAT_GROUP_ORDER, then where the group first appears in the file.
        const auto key = [](int cheat) {
            const auto known = std::find(std::begin(CHEAT_GROUP_ORDER), std::end(CHEAT_GROUP_ORDER), cheat_groups[cheat]);
            const auto first = std::find(cheat_groups.begin(), cheat_groups.end(), cheat_groups[cheat]);
            return std::pair{known - std::begin(CHEAT_GROUP_ORDER), first - cheat_groups.begin()};
        };
        std::vector<int> order(nx::cheats::count());
        for (int cheat = 0; cheat < nx::cheats::count(); cheat++) order[cheat] = cheat;
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return key(a) < key(b); });
        return order;
    }

    const char* row_section(int row) {
        if (is_dev_tool(row)) return dev_tool(row).section;
        if (is_cheat(row)) return "Cheats";
        if (is_mod(row)) return "Mods";
        return nx::info(static_cast<Setting>(row)).section;
    }

    // The header above a row inside its section: the mod's name, the cheat's group; nullptr for none.
    const char* row_group(int row) {
        if (is_mod(row)) return mods[mod_rows[mod_index(row)].mod].display_name.c_str();
        if (is_cheat(row)) return cheat_groups[cheat_index(row)].c_str();
        return nullptr;
    }

    const char* current_section() { return tab_sections[tab].empty() ? "" : tab_sections[tab][tab_section[tab]]; }
    std::string section_key() { return std::string(TAB_NAMES[tab]) + "/" + current_section(); }

    std::vector<int> section_rows() {
        std::vector<int> rows;
        for (int row : tab_rows[tab]) {
            if (std::strcmp(row_section(row), current_section()) == 0) rows.push_back(row);
        }
        return rows;
    }

    void build_tabs() {
        for (std::vector<int>& rows : tab_rows) rows.clear();
        for (Setting setting : SETTINGS_TAB) tab_rows[TAB_SETTINGS].push_back(static_cast<int>(setting));
        for (Setting setting : ENHANCEMENTS_TAB) tab_rows[TAB_ENHANCEMENTS].push_back(static_cast<int>(setting));
        for (int mod = 0; mod < mod_count(); mod++) tab_rows[TAB_ENHANCEMENTS].push_back(SETTING_COUNT + mod);
        for (int cheat : cheats_in_order()) tab_rows[TAB_ENHANCEMENTS].push_back(SETTING_COUNT + mod_count() + cheat);
        for (int tool = 0; tool < nx::dev::count(); tool++) tab_rows[TAB_DEV_TOOLS].push_back(system_row() + 2 + tool);
        for (int i = 0; i < TAB_COUNT; i++) {
            tab_sections[i].clear();
            for (int row : tab_rows[i]) {
                const char* section = row_section(row);
                const auto same = [&](const char* other) { return std::strcmp(other, section) == 0; };
                if (std::none_of(tab_sections[i].begin(), tab_sections[i].end(), same)) tab_sections[i].push_back(section);
            }
            tab_section[i] = std::clamp(tab_section[i], 0, std::max(0, static_cast<int>(tab_sections[i].size()) - 1));
        }
        if (tab_rows[tab].empty()) tab = TAB_SETTINGS;
        // The focused row may be gone (mods or cheats changed on the card): back to the sections then.
        const std::vector<int> rows = section_rows();
        if (zone == Zone::Content && std::find(rows.begin(), rows.end(), selected) == rows.end()) zone = Zone::Sidebar;
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

    // Quit Game, or Restart Game: the homebrew loader then starts the .nro again.
    void leave_game(bool restart) {
        save_changes();
        if (restart) {
            const Result rc = envSetNextLoad(dk64nx_program_path, dk64nx_program_path);
            std::printf("[DK64NX] restart: next load %s (rc 0x%x)\n", dk64nx_program_path, rc);
        }
        SDL_Event quit{};
        quit.type = SDL_QUIT;
        SDL_PushEvent(&quit);
    }

    std::atomic<bool> restart_requested{false};  // dev harness (restart command)

    // The left stick acts as the D-pad along its dominant axis only (a diagonal would move and change a value at
    // once), pressed past STICK_PRESS and released below STICK_RELEASE.
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

    // Reopens where it was left: the same tab, section and row (the top bar's buttons excepted).
    void open_menu() {
        refresh_mods();
        build_tabs();
        is_open = true;
        owns_pad = true;
        release_pending = false;
        changed = false;
        modal = Modal::None;
        if (zone == Zone::Toolbar) zone = Zone::Sidebar;
        scroll_to_selection = true;
    }

    void close_menu() {
        is_open = false;
        release_pending = true;
        modal = Modal::None;
        save_changes();
    }

    // ---- Rows: their widget, value and texts ----

    bool is_switch(const nx::SettingInfo& info) {
        if (info.choices.size() != 2) return false;
        const char* first = info.choices.begin()[0];
        const char* second = info.choices.begin()[1];
        return (std::strcmp(first, "Off") == 0 && std::strcmp(second, "On") == 0) ||
               (std::strcmp(first, "On") == 0 && std::strcmp(second, "Off") == 0);
    }

    Widget row_widget(int row) {
        if (is_dev_tool(row)) {
            if (dev_tool(row).change == nullptr) return Widget::Text;
            if (dev_tool(row).button) return Widget::Button;
            const std::string value = dev_tool(row).value();
            return value == "On" || value == "Off" ? Widget::Checkbox : Widget::Stepper;
        }
        if (is_cheat(row)) return nx::cheats::supported(cheat_index(row)) ? Widget::Checkbox : Widget::Text;
        if (is_mod(row)) {
            const ModRow& mod_row = mod_rows[mod_index(row)];
            if (mod_row.option < 0) return Widget::Checkbox;
            switch (mod_option(mod_row).type) {
                case recomp::config::ConfigOptionType::Enum: return Widget::Choice;
                case recomp::config::ConfigOptionType::Number: return Widget::Slider;
                default: return Widget::Checkbox;
            }
        }
        const nx::SettingInfo& info = nx::info(static_cast<Setting>(row));
        if (info.choices.size() == 0) return Widget::Slider;
        return is_switch(info) ? Widget::Checkbox : Widget::Choice;
    }

    std::string value_text(int row) {
        if (is_dev_tool(row)) return dev_tool(row).value();
        if (is_cheat(row)) {
            const int cheat = cheat_index(row);
            return !nx::cheats::supported(cheat) ? "Unsupported" : nx::cheats::enabled(cheat) ? "On" : "Off";
        }
        if (is_mod(row)) {
            const ModRow& mod_row = mod_rows[mod_index(row)];
            if (mod_row.option >= 0) return mod_option_text(mod_row);
            const std::string& id = mods[mod_row.mod].mod_id;
            return mod_chosen[id] || mod_needed(id) ? "On" : "Off";
        }
        const Setting setting = static_cast<Setting>(row);
        const nx::SettingInfo& info = nx::info(setting);
        const int value = nx::get(setting);
        return info.choices.size() > 0 ? info.choices.begin()[value] : std::to_string(value) + info.suffix;
    }

    bool row_checked(int row) { return value_text(row) == "On"; }

    // The knob of a slider, 0 at the minimum and 1 at the maximum.
    float row_fraction(int row) {
        if (is_mod(row)) {
            const ModRow& mod_row = mod_rows[mod_index(row)];
            const auto& range = std::get<recomp::config::ConfigOptionNumber>(mod_option(mod_row).variant);
            recomp::config::Config* config = recomp::mods::get_mod_config(mods[mod_row.mod].mod_id);
            const auto value = config != nullptr ? config->get_option_value(mod_option(mod_row).id) : recomp::config::ConfigValueVariant{};
            const double* real = std::get_if<double>(&value);
            return real == nullptr || range.max <= range.min ? 0.0f : static_cast<float>((*real - range.min) / (range.max - range.min));
        }
        const nx::SettingInfo& info = nx::info(static_cast<Setting>(row));
        if (info.max <= info.min) return 0.0f;
        return static_cast<float>(nx::get(static_cast<Setting>(row)) - info.min) / static_cast<float>(info.max - info.min);
    }

    // The choices of a Choice row and the current one, for its list.
    std::vector<const char*> row_choices(int row, int* current) {
        std::vector<const char*> items;
        *current = 0;
        if (is_mod(row)) {
            const ModRow& mod_row = mod_rows[mod_index(row)];
            const auto& choices = std::get<recomp::config::ConfigOptionEnum>(mod_option(mod_row).variant).options;
            const std::string text = mod_option_text(mod_row);
            for (size_t i = 0; i < choices.size(); i++) {
                items.push_back(choices[i].name.c_str());
                if (choices[i].name == text) *current = static_cast<int>(i);
            }
            return items;
        }
        for (const char* choice : nx::info(static_cast<Setting>(row)).choices) items.push_back(choice);
        *current = nx::get(static_cast<Setting>(row));
        return items;
    }

    // Applies at the next start: a display setting changed since, or a mod turned on or off.
    bool restart_pending(int row) {
        if (is_mod(row) && mod_rows[mod_index(row)].option < 0) {
            const std::string& id = mods[mod_rows[mod_index(row)].mod].mod_id;
            return (mod_chosen[id] || mod_needed(id)) != mod_enabled_at_start[id];
        }
        if (!is_setting(row)) return false;
        const Setting setting = static_cast<Setting>(row);
        return nx::info(setting).needs_restart && nx::get(setting) != nx::at_startup(setting);
    }

    bool applies_at_restart(int row) {
        return (is_mod(row) && mod_rows[mod_index(row)].option < 0) ||
               (is_setting(row) && nx::info(static_cast<Setting>(row)).needs_restart);
    }

    const char* row_label(int row) {
        if (is_dev_tool(row)) return dev_tool(row).label;
        if (is_cheat(row)) {
            const char* name = nx::cheats::name(cheat_index(row));
            const char* separator = std::strstr(name, CHEAT_SEPARATOR);
            return separator == nullptr ? name : separator + std::strlen(CHEAT_SEPARATOR);
        }
        if (is_mod(row)) {
            const ModRow& mod_row = mod_rows[mod_index(row)];
            return mod_row.option < 0 ? "Enabled" : mod_option(mod_row).name.c_str();
        }
        return nx::info(static_cast<Setting>(row)).label;
    }

    const char* row_description(int row) {
        if (is_dev_tool(row)) return dev_tool(row).description;
        if (is_cheat(row)) {
            return nx::cheats::supported(cheat_index(row))
                ? "GameShark code from cheats.cht, written into the game's memory every frame while On."
                : "This code uses a GameShark type the menu does not run.";
        }
        if (is_mod(row) && mod_rows[mod_index(row)].option >= 0) {
            return mod_option(mod_rows[mod_index(row)]).description.c_str();
        }
        if (is_mod(row)) {
            const recomp::mods::ModDetails& mod = mods[mod_rows[mod_index(row)].mod];
            mod_description = mod.short_description.empty() ? mod.description : mod.short_description;
            if (!mod_chosen[mod.mod_id] && mod_needed(mod.mod_id)) mod_description += " On because another mod needs it.";
            mod_description += " (version " + mod.version.to_string() + ")";
            return mod_description.c_str();
        }
        return nx::info(static_cast<Setting>(row)).description;
    }

    const char* icon_description(Icon which) {
        switch (which) {
            case Icon::Help: return "The buttons of this menu.";
            case Icon::Quit:
                return "Quit Game: closes the game and returns to the homebrew menu. Progress since your last save is lost.";
            case Icon::Restart:
                return can_restart()
                    ? "Restart Game: closes the game and starts it again, to apply mods, the aspect ratio and the "
                      "resolution. Progress since your last save is lost."
                    : "Restart Game: the homebrew loader that started the game cannot start it again; use Quit Game.";
            case Icon::Close: return "Closes this menu, as - and + together.";
        }
        return "";
    }

    // ---- Input ----

    // A cycles through the values and toggles; left/right stop at the ends of a number range.
    void change_selected(int direction, bool wrap) {
        if (selected < 0) return;
        if (is_dev_tool(selected)) {
            if (dev_tool(selected).change != nullptr) dev_tool(selected).change(direction, wrap);
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

    // Into the rows of the chosen section, on the row focused there last time.
    void enter_content() {
        const std::vector<int> rows = section_rows();
        if (rows.empty()) return;
        const auto saved = section_row.find(section_key());
        const bool still_there = saved != section_row.end() && std::find(rows.begin(), rows.end(), saved->second) != rows.end();
        selected = still_there ? saved->second : rows.front();
        zone = Zone::Content;
        scroll_to_selection = true;
    }

    // Up from the first section goes to the top bar's buttons.
    void move_section(int direction) {
        if (direction < 0 && tab_section[tab] == 0) {
            zone = Zone::Toolbar;
            return;
        }
        const int last = static_cast<int>(tab_sections[tab].size()) - 1;
        const int next = std::clamp(tab_section[tab] + direction, 0, std::max(0, last));
        if (next != tab_section[tab]) scroll_to_top = true;
        tab_section[tab] = next;
    }

    void move_selection(int direction) {
        const std::vector<int> rows = section_rows();
        const auto found = std::find(rows.begin(), rows.end(), selected);
        if (found == rows.end()) return;
        const int index = std::clamp(static_cast<int>(found - rows.begin()) + direction, 0, static_cast<int>(rows.size()) - 1);
        selected = rows[index];
        section_row[section_key()] = selected;
        scroll_to_selection = true;
    }

    // L/R: the previous or next tab, on its chosen section. A tab without rows is skipped.
    void switch_tab(int direction) {
        for (int step = 0; step < TAB_COUNT; step++) {
            tab = (tab + direction + TAB_COUNT) % TAB_COUNT;
            if (!tab_rows[tab].empty()) break;
        }
        scroll_to_top = true;
        if (zone == Zone::Content) enter_content();
        else zone = Zone::Sidebar;
    }

    // Left on a row that has no value to move (a checkbox, a readout) goes back to the sections, as in the
    // HarbourMasters menus; otherwise left and right change the value.
    void adjust_row(int direction) {
        const Widget widget = row_widget(selected);
        if (widget == Widget::Choice || widget == Widget::Slider || widget == Widget::Stepper) {
            change_selected(direction, false);
        }
        else if (direction < 0) {
            zone = Zone::Sidebar;
        }
    }

    void activate_row() {
        switch (row_widget(selected)) {
            case Widget::Choice:
                choice_items = row_choices(selected, &choice_index);
                if (!choice_items.empty()) modal = Modal::Choices;
                break;
            case Widget::Checkbox:
            case Widget::Slider:
            case Widget::Stepper:
            case Widget::Button:
                change_selected(1, true);
                break;
            case Widget::Text:
                break;
        }
    }

    void apply_choice() {
        if (is_mod(selected)) {
            const ModRow& row = mod_rows[mod_index(selected)];
            const recomp::config::ConfigOption& option = mod_option(row);
            const auto& choices = std::get<recomp::config::ConfigOptionEnum>(option.variant).options;
            if (choice_index < static_cast<int>(choices.size())) {
                recomp::mods::set_mod_config_value(mods[row.mod].mod_id, option.id, choices[choice_index].value);
            }
            return;
        }
        if (!is_setting(selected)) return;
        nx::set(static_cast<Setting>(selected), choice_index);
        changed = true;
    }

    void press_icon() {
        switch (ICONS[icon]) {
            case Icon::Help: modal = Modal::Help; break;
            case Icon::Quit: confirm_restart = false; modal = Modal::Confirm; break;
            case Icon::Restart: confirm_restart = true; modal = Modal::Confirm; break;
            case Icon::Close: close_menu(); break;
        }
    }

    void handle_modal(uint32_t actions) {
        switch (modal) {
            case Modal::Help:
                if (actions & (NX_A | NX_B)) modal = Modal::None;
                break;
            case Modal::Confirm:
                if (actions & NX_B) modal = Modal::None;
                else if ((actions & NX_A) && (!confirm_restart || can_restart())) leave_game(confirm_restart);
                break;
            case Modal::Choices: {
                const int last = static_cast<int>(choice_items.size()) - 1;
                if (actions & NX_UP) choice_index = std::max(0, choice_index - 1);
                if (actions & NX_DOWN) choice_index = std::min(last, choice_index + 1);
                if (actions & NX_A) {
                    apply_choice();
                    modal = Modal::None;
                }
                if (actions & NX_B) modal = Modal::None;
                break;
            }
            case Modal::None:
                break;
        }
    }

    void handle_zone_input(uint32_t pressed, uint32_t actions) {
        if (pressed & (NX_L | NX_R)) {
            switch_tab((pressed & NX_L) != 0 ? -1 : 1);
            return;
        }
        switch (zone) {
            case Zone::Sidebar:
                if (actions & NX_B) close_menu();
                else if (actions & NX_UP) move_section(-1);
                else if (actions & NX_DOWN) move_section(1);
                else if (actions & (NX_RIGHT | NX_A)) enter_content();
                break;
            case Zone::Content:
                if (actions & NX_B) zone = Zone::Sidebar;
                else if (actions & NX_UP) move_selection(-1);
                else if (actions & NX_DOWN) move_selection(1);
                else if (actions & NX_LEFT) adjust_row(-1);
                else if (actions & NX_RIGHT) adjust_row(1);
                else if (actions & NX_A) activate_row();
                break;
            case Zone::Toolbar:
                if (actions & (NX_B | NX_DOWN)) zone = Zone::Sidebar;
                else if (actions & NX_LEFT) icon = std::max(0, icon - 1);
                else if (actions & NX_RIGHT) icon = std::min(ICON_COUNT - 1, icon + 1);
                else if (actions & NX_A) press_icon();
                break;
        }
    }

    void handle_zone(uint32_t pressed, uint32_t actions) {
        const Zone before = zone;
        handle_zone_input(pressed, actions);
        // A direction still held after moving to another zone must not repeat there (Right into a section would
        // otherwise go on changing its first row): it repeats again once pressed anew.
        if (zone != before) repeat_at = UINT64_MAX;
    }

    NxPad last_pad;  // read once per frame, also drawn by the Input Viewer

    void update_input(uint64_t now) {
        NxPad pad;
        dk64nx_read_pad(&pad);
        last_pad = pad;
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
        if (modal != Modal::None) {
            handle_modal(actions);
            return;
        }
        handle_zone(pressed, actions);
    }

    void count_frame(uint64_t now) {
        frames_counted++;
        if (now - fps_window_start_ms >= 1000) {
            fps = static_cast<int>(frames_counted * 1000 / (now - fps_window_start_ms));
            frames_counted = 0;
            fps_window_start_ms = now;
        }
    }

    // ---- Drawing ----

    void apply_style() {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 12.0f;
        style.WindowPadding = {22.0f, 18.0f};
        style.WindowBorderSize = 0.0f;
        style.ItemSpacing = {8.0f, 4.0f};
        style.ScrollbarSize = 6.0f;
        style.Colors[ImGuiCol_ChildBg] = {0.0f, 0.0f, 0.0f, 0.0f};
        style.Colors[ImGuiCol_ScrollbarBg] = {0.0f, 0.0f, 0.0f, 0.0f};
        style.Colors[ImGuiCol_ScrollbarGrab] = {1.0f, 1.0f, 1.0f, 0.18f};
    }

    // The atlas is built at the font's pixel size (sharper than scaling one size up or down).
    void load_font(float size) {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        ImFontConfig config;
        if (have_shared_font) {
            config.FontDataOwnedByAtlas = false;
            io.Fonts->AddFontFromMemoryTTF(shared_font.address, static_cast<int>(shared_font.size), size, &config);
        }
        else {
            config.SizePixels = size;
            io.Fonts->AddFontDefault(&config);
        }
        loaded_font_size = size;
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
        have_shared_font = R_SUCCEEDED(plInitialize(PlServiceType_User)) &&
                           R_SUCCEEDED(plGetSharedFontByType(&shared_font, PlSharedFontType_Standard));
        load_font(font_size());
        apply_style();
        // GLideN64 creates a 3.3 core profile context.
        return ImGui_ImplOpenGL3_Init("#version 330 core");
    }

    // The tabs on the left, the icon buttons on the right, a line under them.
    void draw_bar(const Theme& theme) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = px(BAR_HEIGHT) - 4.0f;
        float x = origin.x;
        for (int i = 0; i < TAB_COUNT; i++) {
            if (tab_rows[i].empty()) continue;
            const float tab_width = ImGui::CalcTextSize(TAB_NAMES[i]).x + 32.0f;
            nx::menu::button(draw, {x, origin.y}, {x + tab_width, origin.y + height}, TAB_NAMES[i], i == tab, false, theme);
            x += tab_width + 8.0f;
        }
        const float GAP = px(12.0f);
        float icon_x = origin.x + width - ICON_COUNT * height - (ICON_COUNT - 1) * GAP;
        for (int i = 0; i < ICON_COUNT; i++) {
            const bool enabled = ICONS[i] != Icon::Restart || can_restart();
            nx::menu::icon_button(draw, {icon_x, origin.y}, height, ICONS[i], zone == Zone::Toolbar && icon == i, enabled, theme);
            icon_x += height + GAP;
        }
        const float line_y = origin.y + px(BAR_HEIGHT) + 6.0f;
        draw->AddLine({origin.x, line_y}, {origin.x + width, line_y}, theme.line, 2.0f);
        ImGui::Dummy({width, px(BAR_HEIGHT) + 16.0f});
    }

    void draw_sidebar(const Theme& theme, float height) {
        ImGui::BeginChild("sections", {px(SIDEBAR_WIDTH), height});
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (size_t i = 0; i < tab_sections[tab].size(); i++) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 size{px(SIDEBAR_WIDTH) - 12.0f, px(SECTION_HEIGHT)};
            ImGui::Dummy(size);
            const bool chosen = static_cast<int>(i) == tab_section[tab];
            nx::menu::button(draw, {at.x + 3.0f, at.y + 3.0f}, {at.x + size.x, at.y + size.y - 3.0f}, tab_sections[tab][i],
                             chosen, chosen && zone == Zone::Sidebar, theme);
        }
        ImGui::EndChild();
    }

    float row_height() { return ImGui::GetTextLineHeight() + 2 * px(ROW_PADDING); }

    // The width a row's box needs (choices, sliders, steppers, buttons); 0 for a checkbox or a readout.
    float needed_width(int row) {
        const float box = row_height() - 8.0f;
        const float text_width = ImGui::CalcTextSize(value_text(row).c_str()).x;
        switch (row_widget(row)) {
            case Widget::Choice: return std::max(px(CHOICE_MIN_WIDTH), text_width + box + 30.0f);
            case Widget::Slider: return px(SLIDER_WIDTH);
            case Widget::Stepper: return std::max(px(STEPPER_MIN_WIDTH), text_width + 2 * box + 36.0f);
            case Widget::Button: return std::max(px(STEPPER_MIN_WIDTH), text_width + 40.0f);
            default: return 0.0f;
        }
    }

    // The widget of a row, right-aligned at right, its box column wide (the section's widest, so the boxes of a
    // section line up); returns its left edge.
    float draw_widget(ImDrawList* draw, int row, ImVec2 start, float right, float column, bool focused, const Theme& theme) {
        const float box = row_height() - 8.0f;
        const float top = start.y + 4.0f;
        const float left = right - column;
        const std::string value = value_text(row);
        const float text_width = ImGui::CalcTextSize(value.c_str()).x;
        switch (row_widget(row)) {
            case Widget::Checkbox:
                nx::menu::checkbox(draw, {right - box, top}, box, row_checked(row), focused, theme);
                return right - box;
            case Widget::Choice:
                nx::menu::choice_box(draw, {left, top}, {right, top + box}, value.c_str(), focused, theme);
                if (focused) {
                    choice_box_min = {left, top};
                    choice_box_max = {right, top + box};
                }
                return left;
            case Widget::Slider:
                nx::menu::slider(draw, {left, top}, {right, top + box}, row_fraction(row), value.c_str(), focused, theme);
                return left;
            case Widget::Stepper:
                nx::menu::slider(draw, {left, top}, {right, top + box}, -1.0f, value.c_str(), focused, theme);
                return left;
            case Widget::Button:
                nx::menu::push_button(draw, {left, top}, {right, top + box}, value.c_str(), focused, theme);
                return left;
            case Widget::Text:
                draw->AddText({right - text_width, start.y + px(ROW_PADDING)}, theme.text_dim, value.c_str());
                return right - text_width;
        }
        return right;
    }

    void draw_row(int row, float column, const Theme& theme) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = row_height();
        ImGui::Dummy({width, height});
        const bool focused = zone == Zone::Content && row == selected;
        const ImVec2 end{start.x + width, start.y + height};
        if (focused) {
            if (scroll_to_selection) {
                ImGui::SetScrollHereY(0.5f);
                scroll_to_selection = false;
            }
            draw->AddRectFilled(start, end, IM_COL32(255, 255, 255, 18), 6.0f);
            // A readout has no box to outline: the row is.
            if (row_widget(row) == Widget::Text) draw->AddRect(start, end, theme.focus, 6.0f, 0, 2.0f);
        }
        float left = draw_widget(draw, row, start, end.x - 12.0f, column, focused, theme);
        if (restart_pending(row)) {
            left -= ImGui::CalcTextSize("*").x + 12.0f;
            draw->AddText({left, start.y + px(ROW_PADDING)}, nx::menu::RESTART_MARK, "*");
        }
        // Long labels (cheats) end with an ellipsis before the widget instead of running into it.
        const ImVec2 label_min{start.x + 14.0f, start.y + px(ROW_PADDING)};
        const ImVec2 label_max{left - 16.0f, end.y};
        ImGui::PushStyleColor(ImGuiCol_Text, theme.text);
        ImGui::RenderTextEllipsis(draw, label_min, label_max, label_max.x, label_max.x, row_label(row), nullptr, nullptr);
        ImGui::PopStyleColor();
    }

    void draw_content(const Theme& theme, float height) {
        ImGui::BeginChild("rows", {0.0f, height});
        if (scroll_to_top) {
            ImGui::SetScrollY(0.0f);
            scroll_to_top = false;
        }
        const std::vector<int> rows = section_rows();
        float column = 0.0f;
        for (int row : rows) column = std::max(column, needed_width(row));
        const char* group = nullptr;
        for (int row : rows) {
            const char* row_group_name = row_group(row);
            if (row_group_name != nullptr && (group == nullptr || std::strcmp(group, row_group_name) != 0)) {
                ImGui::Dummy({0.0f, 4.0f});
                nx::menu::group_header(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(),
                                       ImGui::GetContentRegionAvail().x - 12.0f, row_group_name, theme);
                ImGui::Dummy({0.0f, ImGui::GetTextLineHeight() + 2.0f});
            }
            group = row_group_name;
            draw_row(row, column, theme);
        }
        ImGui::EndChild();
    }

    void draw_footer(const Theme& theme) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        draw->AddLine({at.x, at.y + 4.0f}, {at.x + ImGui::GetContentRegionAvail().x, at.y + 4.0f}, theme.line, 1.0f);
        ImGui::Dummy({0.0f, 10.0f});
        const char* description = zone == Zone::Content && selected >= 0 ? row_description(selected)
                                  : zone == Zone::Toolbar               ? icon_description(ICONS[icon])
                                                                        : nullptr;
        // A longer description is cut at three lines rather than running over the restart note and the hints.
        const bool note = zone == Zone::Content && selected >= 0 && applies_at_restart(selected);
        if (description != nullptr) {
            const ImVec2 top = ImGui::GetCursorScreenPos();
            const float bottom = top.y + 3 * ImGui::GetTextLineHeightWithSpacing();
            ImGui::PushClipRect(top, {top.x + ImGui::GetContentRegionAvail().x, bottom}, true);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(description);
            ImGui::PopTextWrapPos();
            ImGui::PopClipRect();
            ImGui::SetCursorScreenPos({top.x, std::min(ImGui::GetCursorScreenPos().y, bottom)});
        }
        if (note) {
            ImGui::PushStyleColor(ImGuiCol_Text, nx::menu::RESTART_MARK);
            ImGui::TextUnformatted("* Applies the next time the game starts.");
            ImGui::PopStyleColor();
        }
        const char* hint = zone == Zone::Sidebar   ? "Up/Down  Section    A  Open    L/R  Tab    B  Close"
                           : zone == Zone::Content ? "Up/Down  Select    Left/Right  Change    A  Toggle or choose    B  Back"
                                                   : "Left/Right  Choose    A  Select    B  Back";
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - ImGui::GetTextLineHeight());
        ImGui::PushStyleColor(ImGuiCol_Text, theme.text_dim);
        ImGui::TextUnformatted(hint);
        ImGui::PopStyleColor();
    }

    void draw_modal(ImVec2 screen, const Theme& theme) {
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        switch (modal) {
            case Modal::Choices:
                nx::menu::choice_list(draw, choice_box_min, choice_box_max, screen.y, choice_items, choice_index, theme);
                break;
            case Modal::Confirm:
                if (confirm_restart && !can_restart()) {
                    nx::menu::dialog(draw, screen, "Restart Game",
                                     {"The homebrew loader that started the game", "cannot start it again: use Quit Game."},
                                     "B  Back", theme);
                }
                else {
                    nx::menu::dialog(draw, screen, confirm_restart ? "Restart the game?" : "Quit the game?",
                                     {confirm_restart ? "It closes and starts again, applying mods and display settings."
                                                      : "It closes and returns to the homebrew menu.",
                                      "Progress since your last save is lost."},
                                     "A  Yes        B  No", theme);
                }
                break;
            case Modal::Help:
                nx::menu::dialog(draw, screen, "Buttons",
                                 {"-  and  +          Open or close this menu", "L / R               Previous or next tab",
                                  "Up / Down        Section, then setting", "A                     Open, toggle, choose",
                                  "Left / Right      Change a value", "B                     Back, then close",
                                  "Up on the first section reaches these buttons."},
                                 "A or B  Close", theme);
                break;
            case Modal::None:
                break;
        }
    }

    void draw_menu(float screen_width, float screen_height) {
        const Theme& theme = look();
        ImGuiStyle& style = ImGui::GetStyle();
        ImVec4 background = theme.panel;
        background.w = nx::get(Setting::MenuOpacity) / 100.0f;
        style.Colors[ImGuiCol_WindowBg] = background;
        style.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(theme.text);
        ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, {screen_width, screen_height},
                                                      IM_COL32(0, 0, 0, 150));
        const ImVec2 panel{screen_width * PANEL_SHARE.x, screen_height * PANEL_SHARE.y};
        ImGui::SetNextWindowPos({(screen_width - panel.x) / 2, (screen_height - panel.y) / 2});
        ImGui::SetNextWindowSize(panel);
        ImGui::Begin("NX Options", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
        draw_bar(theme);
        const float footer_height = FOOTER_LINES * ImGui::GetTextLineHeightWithSpacing() + 20.0f;
        const float body_height = ImGui::GetContentRegionAvail().y - footer_height;
        draw_sidebar(theme, body_height);
        ImGui::SameLine(0.0f, 0.0f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine({at.x + 6.0f, at.y}, {at.x + 6.0f, at.y + body_height}, theme.line, 2.0f);
        ImGui::SetCursorScreenPos({at.x + 22.0f, at.y});
        draw_content(theme, body_height);
        draw_footer(theme);
        ImGui::End();
        draw_modal({screen_width, screen_height}, theme);
    }

    // FPS, the passes of the last frame and the screen refreshes per second that showed nothing new.
    int dropped_per_second = 0;
    int dropped_window_start = 0;

    void count_dropped(uint64_t now) {
        static uint64_t window_start_ms = 0;
        if (now - window_start_ms < 1000) return;
        const int dropped = dk64nx_dropped_screen_updates.load();
        dropped_per_second = dropped - dropped_window_start;
        dropped_window_start = dropped;
        window_start_ms = now;
    }

    void draw_fps() {
        char text[96];
        if (nx::get(Setting::PerformanceOverlay) != 0) {
            std::snprintf(text, sizeof(text), "%d FPS   %.1f + %.1f ms   %d skipped/s   %s", fps,
                          dk64nx_pass1_us.load() / 1000.0f, dk64nx_pass2_us.load() / 1000.0f, dropped_per_second,
                          dk64nx_vsync_paced.load() ? "vsync" : "timer");
        }
        else {
            std::snprintf(text, sizeof(text), "%d FPS", fps);
        }
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddRectFilled({12.0f, 12.0f}, {28.0f + size.x, 20.0f + size.y}, IM_COL32(0, 0, 0, 160), 6.0f);
        draw->AddText({20.0f, 16.0f}, OVERLAY_TEXT, text);
    }

    // Settings > Overlays > Input Viewer, bottom right: the face buttons, the shoulders, the D-pad, - and +, and each
    // stick's position in its circle; pressed buttons in the theme's accent.
    void draw_input_viewer(ImVec2 screen) {
        const nx::menu::Theme& theme = look();
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        const uint32_t b = last_pad.buttons;
        const float unit = 14.0f;
        const ImVec2 origin{screen.x - 24.0f - 22 * unit, screen.y - 24.0f - 11 * unit};
        const ImU32 off = IM_COL32(40, 40, 44, 200);
        const ImU32 edge = IM_COL32(230, 230, 230, 160);
        draw->AddRectFilled({origin.x - 10, origin.y - 10}, {origin.x + 22 * unit + 10, origin.y + 11 * unit + 10},
                            IM_COL32(0, 0, 0, 120), 10.0f);
        const auto key = [&](float x, float y, float w, float h, uint32_t mask, const char* text) {
            const ImVec2 min{origin.x + x * unit, origin.y + y * unit};
            const ImVec2 max{min.x + w * unit, min.y + h * unit};
            draw->AddRectFilled(min, max, (b & mask) ? theme.accent : off, 5.0f);
            draw->AddRect(min, max, edge, 5.0f);
            const ImVec2 size = ImGui::CalcTextSize(text);
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.6f,
                          {(min.x + max.x - size.x * 0.6f) / 2, (min.y + max.y - size.y * 0.6f) / 2},
                          (b & mask) ? theme.on_accent : OVERLAY_TEXT, text);
        };
        const auto stick = [&](float cx, float cy, float x, float y, uint32_t mask) {
            const ImVec2 c{origin.x + cx * unit, origin.y + cy * unit};
            const float r = 2.6f * unit;
            draw->AddCircleFilled(c, r, (b & mask) ? theme.box : off, 32);
            draw->AddCircle(c, r, edge, 32, 1.5f);
            draw->AddCircleFilled({c.x + x * (r - 6), c.y - y * (r - 6)}, 6.0f, theme.knob, 16);
        };
        key(0, 0, 4, 1.5f, NX_ZL, "ZL");  key(4.5f, 0, 4, 1.5f, NX_L, "L");
        key(13.5f, 0, 4, 1.5f, NX_R, "R"); key(18, 0, 4, 1.5f, NX_ZR, "ZR");
        stick(3, 5, last_pad.lx, last_pad.ly, NX_LSTICK);
        stick(15.5f, 8.2f, last_pad.rx, last_pad.ry, NX_RSTICK);
        key(9, 2.2f, 1.6f, 1.6f, NX_MINUS, "-"); key(11.4f, 2.2f, 1.6f, 1.6f, NX_PLUS, "+");
        key(6.2f, 6.6f, 1.6f, 1.6f, NX_UP, "^"); key(6.2f, 9.4f, 1.6f, 1.6f, NX_DOWN, "v");
        key(4.8f, 8.0f, 1.6f, 1.6f, NX_LEFT, "<"); key(7.6f, 8.0f, 1.6f, 1.6f, NX_RIGHT, ">");
        key(19.6f, 4.6f, 1.8f, 1.8f, NX_A, "A"); key(17.8f, 6.4f, 1.8f, 1.8f, NX_B, "B");
        key(17.8f, 2.8f, 1.8f, 1.8f, NX_X, "X"); key(16.0f, 4.6f, 1.8f, 1.8f, NX_Y, "Y");
    }

    // Settings > Overlays > Gameplay Timer, top right: the file's play time while playing an adventure.
    void draw_gameplay_timer(float screen_width) {
        const long seconds = nx::dev::play_time_seconds();
        if (seconds < 0) return;
        char text[32];
        std::snprintf(text, sizeof(text), "%ld:%02ld:%02ld", seconds / 3600, seconds / 60 % 60, seconds % 60);
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        const ImVec2 size = ImGui::CalcTextSize(text);
        const float left = screen_width - 28.0f - size.x;
        draw->AddRectFilled({left - 8.0f, 12.0f}, {screen_width - 12.0f, 20.0f + size.y}, IM_COL32(0, 0, 0, 160), 6.0f);
        draw->AddText({left, 16.0f}, OVERLAY_TEXT, text);
    }

    // Dev Tools > Game > Map & Position Overlay, bottom left.
    void draw_position(float screen_height) {
        const std::string text = nx::dev::overlay_text();
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const float top = screen_height - 20.0f - size.y;
        draw->AddRectFilled({12.0f, top - 4.0f}, {28.0f + size.x, top + size.y + 4.0f}, IM_COL32(0, 0, 0, 160), 6.0f);
        draw->AddText({20.0f, top}, OVERLAY_TEXT, text.c_str());
    }

    // GLideN64 keeps its own GL state cache: everything the ImGui backend does not restore itself is put back
    // here, so the next emulated frame finds GL exactly as it left it.
    void render(int width, int height, uint64_t now) {
        GLint draw_framebuffer = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

        ImGuiIO& io = ImGui::GetIO();
        if (font_size() != loaded_font_size) {
            load_font(font_size());
            ImGui_ImplOpenGL3_DestroyFontsTexture();
            ImGui_ImplOpenGL3_CreateFontsTexture();
        }
        io.DisplaySize = {static_cast<float>(width), static_cast<float>(height)};
        io.DeltaTime = std::max(0.001f, (now - last_frame_ms) / 1000.0f);
        last_frame_ms = now;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        if (is_open) draw_menu(io.DisplaySize.x, io.DisplaySize.y);
        if (nx::get(Setting::ShowFps) != 0 || nx::get(Setting::PerformanceOverlay) != 0) draw_fps();
        if (nx::dev::overlay_shown() && !is_open) draw_position(io.DisplaySize.y);  // the menu shows the same rows
        if (nx::get(Setting::InputViewer) != 0 && !is_open) draw_input_viewer(io.DisplaySize);
        if (nx::get(Setting::GameplayTimer) != 0 && !is_open) draw_gameplay_timer(io.DisplaySize.x);
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_framebuffer));
    }
}

bool dk64nx_menu_owns_pad() {
    return owns_pad.load();
}

// Graphics thread, right before the swap (declared inside core_switch.cpp's extern "C" block).
extern "C" void dk64nx_menu_request_restart() {
    restart_requested = true;
}

extern "C" void dk64nx_menu_on_swap(int width, int height) {
    if (restart_requested.exchange(false) && can_restart()) {
        leave_game(true);
    }
    const uint64_t now = SDL_GetTicks64();
    count_frame(now);
    count_dropped(now);
    if (is_open) nx::dev::update(now);
    update_input(now);
    nx::cheats::apply();
    if (!is_open && nx::get(Setting::ShowFps) == 0 && nx::get(Setting::PerformanceOverlay) == 0 &&
        nx::get(Setting::InputViewer) == 0 && nx::get(Setting::GameplayTimer) == 0 && !nx::dev::overlay_shown()) {
        return;
    }
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
