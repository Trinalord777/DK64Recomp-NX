#include "nx_menu_widgets.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <tuple>

#include "imgui_internal.h"  // RenderCheckMark, RenderTextEllipsis

namespace {
    using nx::menu::Theme;

    // Orange is the menu's look since v1.0, Gray a stone gray; both after DK64's own menus.
    const Theme THEMES[] = {
        {{0.07f, 0.07f, 0.08f, 0.94f}, IM_COL32(24, 24, 27, 255), IM_COL32(255, 145, 30, 255), IM_COL32(30, 20, 8, 255),
         IM_COL32(58, 58, 64, 255), IM_COL32(255, 145, 30, 255), IM_COL32(255, 185, 90, 255),
         IM_COL32(235, 235, 235, 255), IM_COL32(150, 150, 150, 255), IM_COL32(130, 130, 138, 255)},
        {{0.09f, 0.09f, 0.10f, 0.93f}, IM_COL32(30, 30, 33, 255), IM_COL32(112, 112, 116, 255), IM_COL32(245, 245, 245, 255),
         IM_COL32(82, 82, 86, 255), IM_COL32(205, 205, 205, 255), IM_COL32(240, 240, 240, 255),
         IM_COL32(240, 240, 240, 255), IM_COL32(165, 165, 165, 255), IM_COL32(150, 150, 155, 255)},
    };
    constexpr float ROUNDING = 5.0f;
    constexpr float KNOB_WIDTH = 12.0f;
    constexpr ImU32 CHECK_MARK = IM_COL32(20, 20, 20, 255);
    constexpr ImU32 RED = IM_COL32(150, 14, 14, 255);
    constexpr ImU32 ICON_GRAY = IM_COL32(105, 105, 110, 255);
    constexpr ImU32 WHITE = IM_COL32(255, 255, 255, 255);

    ImU32 outline(bool focused, const Theme& theme) { return focused ? theme.focus : theme.line; }
    float outline_width(bool focused) { return focused ? 2.5f : 1.0f; }

    void centered_text(ImDrawList* draw, ImVec2 min, ImVec2 max, const char* text, ImU32 color) {
        const ImVec2 size = ImGui::CalcTextSize(text);
        draw->AddText({std::floor((min.x + max.x - size.x) / 2), std::floor((min.y + max.y - size.y) / 2)}, color, text);
    }

    // Centered text with a dark shadow, readable over the slider's fill.
    void shadowed_text(ImDrawList* draw, ImVec2 min, ImVec2 max, const char* text, ImU32 color) {
        centered_text(draw, {min.x + 1.0f, min.y + 1.0f}, {max.x + 1.0f, max.y + 1.0f}, text, IM_COL32(0, 0, 0, 200));
        centered_text(draw, min, max, text, color);
    }

    void framed_box(ImDrawList* draw, ImVec2 min, ImVec2 max, ImU32 fill, bool focused, const Theme& theme) {
        draw->AddRectFilled(min, max, fill, ROUNDING);
        draw->AddRect(min, max, outline(focused, theme), ROUNDING, 0, outline_width(focused));
    }
}

const Theme& nx::menu::theme(int index) {
    return THEMES[std::clamp(index, 0, static_cast<int>(std::size(THEMES)) - 1)];
}

void nx::menu::button(ImDrawList* draw, ImVec2 min, ImVec2 max, const char* text, bool selected, bool focused,
                      const Theme& theme) {
    if (selected) draw->AddRectFilled(min, max, theme.accent, ROUNDING + 1.0f);
    if (focused) draw->AddRect(min, max, theme.focus, ROUNDING + 1.0f, 0, 2.5f);
    centered_text(draw, min, max, text, selected ? theme.on_accent : theme.text);
}

void nx::menu::push_button(ImDrawList* draw, ImVec2 min, ImVec2 max, const char* text, bool focused, const Theme& theme) {
    framed_box(draw, min, max, theme.box, focused, theme);
    centered_text(draw, min, max, text, theme.text);
}

void nx::menu::checkbox(ImDrawList* draw, ImVec2 min, float size, bool checked, bool focused, const Theme& theme) {
    framed_box(draw, min, {min.x + size, min.y + size}, checked ? theme.knob : theme.box, focused, theme);
    if (checked) {
        const float pad = size * 0.2f;
        ImGui::RenderCheckMark(draw, {min.x + pad, min.y + pad}, CHECK_MARK, size - 2 * pad);
    }
}

void nx::menu::choice_box(ImDrawList* draw, ImVec2 min, ImVec2 max, const char* text, bool focused, const Theme& theme) {
    framed_box(draw, min, max, theme.box, focused, theme);
    const float height = max.y - min.y;
    const float arrow = height * 0.22f;
    const ImVec2 center{max.x - height / 2, (min.y + max.y) / 2};
    draw->AddTriangleFilled({center.x - arrow, center.y - arrow / 2}, {center.x + arrow, center.y - arrow / 2},
                            {center.x, center.y + arrow / 2 + 2.0f}, theme.text);
    // The text stops before the arrow and ends with an ellipsis when too long.
    const float text_height = ImGui::CalcTextSize(text).y;
    const ImVec2 text_min{min.x + 10.0f, std::floor((min.y + max.y - text_height) / 2)};
    const ImVec2 text_max{max.x - height - 4.0f, max.y};
    ImGui::PushStyleColor(ImGuiCol_Text, theme.text);
    ImGui::RenderTextEllipsis(draw, text_min, text_max, text_max.x, text_max.x, text, nullptr, nullptr);
    ImGui::PopStyleColor();
}

void nx::menu::slider(ImDrawList* draw, ImVec2 min, ImVec2 max, float fraction, const char* text, bool focused,
                      const Theme& theme) {
    const float side = max.y - min.y;
    const ImVec2 minus_max{min.x + side, max.y};
    const ImVec2 plus_min{max.x - side, min.y};
    for (const auto& [box_min, box_max, sign] : {std::tuple{min, minus_max, "-"}, std::tuple{plus_min, max, "+"}}) {
        framed_box(draw, box_min, box_max, theme.box, focused, theme);
        centered_text(draw, box_min, box_max, sign, theme.text);
    }
    const ImVec2 bar_min{minus_max.x + 6.0f, min.y};
    const ImVec2 bar_max{plus_min.x - 6.0f, max.y};
    framed_box(draw, bar_min, bar_max, theme.box, focused, theme);
    if (fraction >= 0.0f) {
        // The filled part, translucent so the value stays readable over it, then a thin knob at its end.
        const float x = bar_min.x + 3.0f + std::clamp(fraction, 0.0f, 1.0f) * (bar_max.x - bar_min.x - 6.0f - KNOB_WIDTH);
        const ImU32 fill = (theme.knob & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 70);
        draw->AddRectFilled({bar_min.x + 3.0f, bar_min.y + 3.0f}, {x + KNOB_WIDTH / 2, bar_max.y - 3.0f}, fill, ROUNDING - 1.0f);
        draw->AddRectFilled({x + KNOB_WIDTH / 2 - 2.0f, bar_min.y + 3.0f}, {x + KNOB_WIDTH / 2 + 2.0f, bar_max.y - 3.0f}, theme.knob);
    }
    shadowed_text(draw, bar_min, bar_max, text, theme.text);
}

void nx::menu::group_header(ImDrawList* draw, ImVec2 min, float width, const char* text, const Theme& theme) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float y = std::floor(min.y + size.y / 2);
    draw->AddLine({min.x, y}, {min.x + 16.0f, y}, theme.line, 1.5f);
    draw->AddText({min.x + 24.0f, min.y}, theme.text_dim, text);
    draw->AddLine({min.x + 32.0f + size.x, y}, {min.x + width, y}, theme.line, 1.5f);
}

void nx::menu::icon_button(ImDrawList* draw, ImVec2 min, float size, Icon icon, bool focused, bool enabled,
                           const Theme& theme) {
    const ImVec2 max{min.x + size, min.y + size};
    const bool red = icon == Icon::Quit || icon == Icon::Restart;
    const ImU32 glyph = enabled ? WHITE : IM_COL32(255, 255, 255, 90);
    draw->AddRectFilled(min, max, red ? RED : ICON_GRAY, ROUNDING);
    if (focused) draw->AddRect({min.x - 3, min.y - 3}, {max.x + 3, max.y + 3}, theme.focus, ROUNDING + 2.0f, 0, 2.5f);
    const ImVec2 center{(min.x + max.x) / 2, (min.y + max.y) / 2};
    const float radius = size * 0.26f;
    switch (icon) {
        case Icon::Help:
            centered_text(draw, min, max, "?", glyph);
            break;
        case Icon::Quit:  // a power symbol: a ring open at the top and a bar through the gap
            draw->PathArcTo({center.x, center.y + 1.0f}, radius, -IM_PI / 2 + 0.6f, 3 * IM_PI / 2 - 0.6f, 24);
            draw->PathStroke(glyph, 0, 2.5f);
            draw->AddLine({center.x, center.y - radius - 2.0f}, {center.x, center.y}, glyph, 2.5f);
            break;
        case Icon::Restart: {  // a circular arrow, its head at the top pointing right
            draw->PathArcTo(center, radius, -IM_PI / 2 + 0.7f, 3 * IM_PI / 2, 24);
            draw->PathStroke(glyph, 0, 2.5f);
            const ImVec2 tip{center.x + 6.0f, center.y - radius};
            draw->AddTriangleFilled(tip, {center.x - 1.0f, center.y - radius - 6.0f}, {center.x - 1.0f, center.y - radius + 6.0f},
                                    glyph);
            break;
        }
        case Icon::Close: {
            const float d = radius * 0.85f;
            draw->AddLine({center.x - d, center.y - d}, {center.x + d, center.y + d}, glyph, 2.5f);
            draw->AddLine({center.x + d, center.y - d}, {center.x - d, center.y + d}, glyph, 2.5f);
            break;
        }
    }
}

void nx::menu::choice_list(ImDrawList* draw, ImVec2 box_min, ImVec2 box_max, float screen_height,
                           const std::vector<const char*>& items, int current, const Theme& theme) {
    const float row = box_max.y - box_min.y + 6.0f;
    float width = box_max.x - box_min.x;
    for (const char* item : items) width = std::max(width, ImGui::CalcTextSize(item).x + 40.0f);
    // A list taller than the screen shows a window of it around the current item (a mod's long enum).
    const int count = static_cast<int>(items.size());
    const int shown = std::max(1, std::min(count, static_cast<int>((screen_height - 16.0f - 8.0f) / row)));
    const int first = std::clamp(current - shown / 2, 0, count - shown);
    const float height = row * static_cast<float>(shown) + 8.0f;
    // Under the box, or above it when the list would leave the screen, and always inside it.
    float top = box_max.y + 4.0f + height <= screen_height - 8.0f ? box_max.y + 4.0f : box_min.y - 4.0f - height;
    top = std::clamp(top, 8.0f, std::max(8.0f, screen_height - 8.0f - height));
    const ImVec2 min{box_max.x - width, top};
    const ImVec2 max{box_max.x, top + height};
    draw->AddRectFilled(min, max, theme.solid, ROUNDING);
    draw->AddRect(min, max, theme.focus, ROUNDING, 0, 2.0f);
    // Arrows at the edges when items are hidden above or below.
    const float arrow = row * 0.15f;
    const float middle = (min.x + max.x) / 2;
    if (first > 0) {
        draw->AddTriangleFilled({middle - arrow, min.y + 2.0f + arrow}, {middle + arrow, min.y + 2.0f + arrow},
                                {middle, min.y + 2.0f}, theme.text_dim);
    }
    if (first + shown < count) {
        draw->AddTriangleFilled({middle - arrow, max.y - 2.0f - arrow}, {middle + arrow, max.y - 2.0f - arrow},
                                {middle, max.y - 2.0f}, theme.text_dim);
    }
    for (int i = first; i < first + shown; i++) {
        const ImVec2 item_min{min.x + 4.0f, min.y + 4.0f + row * static_cast<float>(i - first)};
        const ImVec2 item_max{max.x - 4.0f, item_min.y + row};
        const bool is_current = i == current;
        if (is_current) draw->AddRectFilled(item_min, item_max, theme.accent, ROUNDING);
        const float text_height = ImGui::CalcTextSize(items[i]).y;
        draw->AddText({item_min.x + 10.0f, std::floor((item_min.y + item_max.y - text_height) / 2)},
                      is_current ? theme.on_accent : theme.text, items[i]);
    }
}

void nx::menu::dialog(ImDrawList* draw, ImVec2 screen, const char* title, const std::initializer_list<const char*>& lines,
                      const char* buttons, const Theme& theme) {
    const float line = ImGui::GetTextLineHeightWithSpacing();
    float width = ImGui::CalcTextSize(title).x;
    for (const char* text : lines) width = std::max(width, ImGui::CalcTextSize(text).x);
    width = std::max(width, ImGui::CalcTextSize(buttons).x) + 64.0f;
    const float height = line * static_cast<float>(lines.size() + 3) + 40.0f;
    const ImVec2 min{std::floor((screen.x - width) / 2), std::floor((screen.y - height) / 2)};
    const ImVec2 max{min.x + width, min.y + height};
    draw->AddRectFilled({0.0f, 0.0f}, screen, IM_COL32(0, 0, 0, 110));
    draw->AddRectFilled(min, max, theme.solid, ROUNDING + 3.0f);
    draw->AddRect(min, max, theme.focus, ROUNDING + 3.0f, 0, 2.0f);
    float y = min.y + 20.0f;
    draw->AddText({min.x + 32.0f, y}, theme.focus, title);
    y += line * 1.4f;
    for (const char* text : lines) {
        draw->AddText({min.x + 32.0f, y}, theme.text, text);
        y += line;
    }
    draw->AddText({min.x + 32.0f, max.y - 20.0f - line}, theme.text_dim, buttons);
}
