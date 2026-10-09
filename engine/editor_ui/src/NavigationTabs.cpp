#include <orbit/editor_ui/EditorUi.hpp>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace orbit::editor_ui
{
namespace
{
ElementCategory CategoryForWorkspace(const NavigationIcon icon)
{
    switch (icon)
    {
    case NavigationIcon::Build: return ElementCategory::Meshes;
    case NavigationIcon::Planet:
    case NavigationIcon::Universe: return ElementCategory::Celestial;
    case NavigationIcon::Simulation: return ElementCategory::Simulation;
    case NavigationIcon::Shading: return ElementCategory::Shading;
    case NavigationIcon::Planning: return ElementCategory::Planning;
    case NavigationIcon::Plugins: return ElementCategory::Plugins;
    }
    return ElementCategory::Neutral;
}

// All icons use a 20 x 20 coordinate grid and the same stroke weight.
void DrawNavigationIcon(
    ImDrawList& draw,
    const NavigationIcon icon,
    const ImVec2 origin,
    const f32 scale,
    const ImU32 color)
{
    const auto point = [origin, scale](const f32 x, const f32 y)
    {
        return ImVec2(origin.x + x * scale, origin.y + y * scale);
    };
    const f32 stroke = 1.6F * scale;
    const auto line = [&](const f32 x1, const f32 y1, const f32 x2, const f32 y2)
    {
        draw.AddLine(point(x1, y1), point(x2, y2), color, stroke);
    };
    const auto circle = [&](const f32 x, const f32 y, const f32 radius)
    {
        draw.AddCircle(point(x, y), radius * scale, color, 32, stroke);
    };
    const auto star = [&](const f32 x, const f32 y, const f32 radius)
    {
        line(x - radius, y, x + radius, y);
        line(x, y - radius, x, y + radius);
    };

    switch (icon)
    {
    case NavigationIcon::Build:
        line(10, 2, 18, 6.5F); line(18, 6.5F, 18, 14);
        line(18, 14, 10, 18.5F); line(10, 18.5F, 2, 14);
        line(2, 14, 2, 6.5F); line(2, 6.5F, 10, 2);
        line(2, 6.5F, 10, 11); line(18, 6.5F, 10, 11);
        line(10, 11, 10, 18.5F);
        break;
    case NavigationIcon::Planet:
        circle(10, 10, 6);
        // Tilted elliptical ring, drawn with the same vector stroke.
        for (int segment = 0; segment <= 48; ++segment)
        {
            const f32 angle = static_cast<f32>(segment) / 48.0F *
                2.0F * std::numbers::pi_v<f32>;
            const f32 x = 9.5F * std::cos(angle);
            const f32 y = 2.8F * std::sin(angle);
            draw.PathLineTo(point(10.0F + x * 0.87F - y * 0.50F,
                                  10.0F + x * 0.50F + y * 0.87F));
        }
        draw.PathStroke(color, ImDrawFlags_Closed, stroke);
        break;
    case NavigationIcon::Universe:
        star(7, 7, 5); star(15.5F, 14.5F, 3); star(16, 3, 1.5F);
        draw.AddCircleFilled(point(3, 16), 1.2F * scale, color, 12);
        break;
    case NavigationIcon::Simulation:
        circle(10, 10, 8);
        draw.AddTriangleFilled(point(8, 5.5F), point(8, 14.5F), point(14.5F, 10), color);
        break;
    case NavigationIcon::Shading:
        circle(10, 10, 8);
        draw.PathLineTo(point(10, 2));
        for (int segment = 0; segment <= 24; ++segment)
        {
            const f32 angle = -std::numbers::pi_v<f32> * 0.5F +
                static_cast<f32>(segment) / 24.0F * std::numbers::pi_v<f32>;
            draw.PathLineTo(point(10 + 8 * std::cos(angle), 10 + 8 * std::sin(angle)));
        }
        draw.PathFillConvex(color);
        break;
    case NavigationIcon::Planning:
        line(10, 7, 10, 11); line(4, 11, 16, 11);
        line(4, 11, 4, 14); line(16, 11, 16, 14);
        draw.AddRect(point(7, 2), point(13, 7), color, scale, 0, stroke);
        draw.AddRect(point(1, 14), point(7, 19), color, scale, 0, stroke);
        draw.AddRect(point(13, 14), point(19, 19), color, scale, 0, stroke);
        break;
    case NavigationIcon::Plugins:
        line(6, 2, 6, 6); line(14, 2, 14, 6);
        line(3, 6, 17, 6); line(4, 6, 4, 10);
        line(16, 6, 16, 10);
        draw.PathArcTo(point(10, 10), 6 * scale, 0,
            std::numbers::pi_v<f32>, 24);
        draw.PathStroke(color, 0, stroke);
        line(10, 16, 10, 19);
        break;
    }
}
} // namespace

bool PanelContext::NavigationTabs(
    const std::string_view id,
    const std::span<const NavigationTab> items,
    i32& index)
{
    TraceWidget(id);
    if (items.empty()) return false;
    index = std::clamp(index, 0, static_cast<i32>(items.size()) - 1);
    const f32 scale = CurrentUiScale();
    const f32 gap = 6.0F * scale;
    const f32 height = 40.0F * scale;
    f32 naturalWidth = gap * static_cast<f32>(items.size() - 1U);
    for (const NavigationTab& item : items)
        naturalWidth += ImGui::CalcTextSize(item.label.data(), item.label.data() + item.label.size()).x + 54.0F * scale;
    const f32 available = ImGui::GetContentRegionAvail().x;
    const bool iconsOnly = naturalWidth > available;
    const f32 compactWidth = std::max(1.0F, std::min(44.0F * scale,
        (available - gap * static_cast<f32>(items.size() - 1U)) / static_cast<f32>(items.size())));

    const std::string ownedId(id);
    ImGui::PushID(ownedId.c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0F * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0));
    bool changed = false;
    for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex)
    {
        if (itemIndex != 0U) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(itemIndex));
        const NavigationTab& tab = items[itemIndex];
        const bool selected = static_cast<i32>(itemIndex) == index;
        const f32 labelWidth = ImGui::CalcTextSize(tab.label.data(), tab.label.data() + tab.label.size()).x;
        const f32 width = iconsOnly ? compactWidth : labelWidth + 54.0F * scale;
        ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImVec4 fill = accent;
        fill.w = selected ? 0.17F : 0.0F;
        ImGui::PushStyleColor(ImGuiCol_Button, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(accent.x, accent.y, accent.z, 0.14F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(accent.x, accent.y, accent.z, 0.24F));
        const bool pressed = ImGui::Button("##workspace-tab", ImVec2(width, height));
        ImGui::PopStyleColor(3);
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        ImDrawList& draw = *ImGui::GetWindowDrawList();
        const ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        const ImVec4 muted = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        const ImVec4 inactive(text.x * 0.65F + muted.x * 0.35F,
            text.y * 0.65F + muted.y * 0.35F,
            text.z * 0.65F + muted.z * 0.35F, text.w);
        const math::Float4 categoryColor =
            ElementCategoryColor(CategoryForWorkspace(tab.icon));
        ImVec4 iconTint(categoryColor.x, categoryColor.y,
            categoryColor.z, categoryColor.w);
        iconTint.w = selected || ImGui::IsItemHovered() ? 1.0F : 0.82F;
        const ImU32 iconForeground = ImGui::ColorConvertFloat4ToU32(iconTint);
        const ImU32 textForeground = ImGui::GetColorU32(
            selected || ImGui::IsItemHovered() ? text : inactive);
        DrawNavigationIcon(draw, tab.icon,
            ImVec2(minimum.x + (iconsOnly ? (width - 20.0F * scale) * 0.5F : 12.0F * scale),
                   minimum.y + (height - 20.0F * scale) * 0.5F), scale, iconForeground);
        if (!iconsOnly)
            draw.AddText(ImVec2(minimum.x + 40.0F * scale,
                minimum.y + (height - ImGui::GetFontSize()) * 0.5F), textForeground,
                tab.label.data(), tab.label.data() + tab.label.size());
        if (selected)
            draw.AddRectFilled(ImVec2(minimum.x + 10.0F * scale, maximum.y - 2.0F * scale),
                ImVec2(maximum.x - 10.0F * scale, maximum.y), ImGui::GetColorU32(ImGuiCol_ButtonActive), scale);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.*s workspace", static_cast<int>(tab.label.size()), tab.label.data());
        if (pressed && !selected)
        {
            index = static_cast<i32>(itemIndex);
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopID();
    return changed;
}
} // namespace orbit::editor_ui
