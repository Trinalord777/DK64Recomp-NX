// Widgets of NX Options (nx_menu.cpp), after the HarbourMasters ports' menus (the Banjo and Paper Mario ports): tab
// and section buttons, checkboxes, choice boxes, sliders, group headers, icon buttons and dialogs, drawn with
// ImDrawList since the menu window takes no input and nx_menu.cpp moves the focus itself.
#pragma once

#include <initializer_list>
#include <vector>

#include "imgui.h"

namespace nx::menu {
    struct Theme {
        ImVec4 panel;     // window background, translucent over the game
        ImU32 solid;      // dialogs and the choice list, opaque
        ImU32 accent;     // selected tab and section
        ImU32 on_accent;  // text on the accent
        ImU32 box;        // checkboxes, choice boxes, slider bars, buttons
        ImU32 knob;       // slider knob, checked box
        ImU32 focus;      // outline of the focused item
        ImU32 text;
        ImU32 text_dim;
        ImU32 line;       // separators, group header lines, widget outlines
    };

    // Setting::MenuTheme: 0 Orange, 1 Gray.
    const Theme& theme(int index);
    // The star of the settings that apply at the next start, in both themes.
    constexpr ImU32 RESTART_MARK = IM_COL32(255, 145, 30, 255);

    // A tab or a section: text in a rounded box filled with the accent when selected, outlined when focused.
    void button(ImDrawList* draw, ImVec2 min, ImVec2 max, const char* text, bool selected, bool focused,
                const Theme& theme);
    // A box with its text centered, run by A (Dev Tools actions).
    void push_button(ImDrawList* draw, ImVec2 min, ImVec2 max, const char* text, bool focused, const Theme& theme);
    // A square box of side size at min, with a check mark when checked.
    void checkbox(ImDrawList* draw, ImVec2 min, float size, bool checked, bool focused, const Theme& theme);
    // The current choice in a box with a down arrow on its right.
    void choice_box(ImDrawList* draw, ImVec2 min, ImVec2 max, const char* text, bool focused, const Theme& theme);
    // [-] bar, knob at fraction (0..1), text centered on it [+]. A fraction below 0 draws no knob (a stepper).
    void slider(ImDrawList* draw, ImVec2 min, ImVec2 max, float fraction, const char* text, bool focused,
                const Theme& theme);
    // "-- text -----": a short line, the text, then a line to the end of the width.
    void group_header(ImDrawList* draw, ImVec2 min, float width, const char* text, const Theme& theme);

    enum class Icon { Help, Quit, Restart, Close };
    void icon_button(ImDrawList* draw, ImVec2 min, float size, Icon icon, bool focused, bool enabled,
                     const Theme& theme);
    // The open list of a choice box: under (or above, near the bottom) the box, the current item highlighted.
    void choice_list(ImDrawList* draw, ImVec2 box_min, ImVec2 box_max, float screen_height,
                     const std::vector<const char*>& items, int current, const Theme& theme);
    // A centered box over the menu: a title in the accent, lines of text, then the button hints.
    void dialog(ImDrawList* draw, ImVec2 screen, const char* title, const std::initializer_list<const char*>& lines,
                const char* buttons, const Theme& theme);
}
