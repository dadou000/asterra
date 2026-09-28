#include <orbit/editor_ui/EditorUi.hpp>

#include <imgui.h>

#include <algorithm>
#include <string>

namespace orbit::editor_ui
{
bool PanelContext::SegmentedControl(
    const std::string_view id,
    const std::span<const std::string_view> items,
    i32& index)
{
    TraceWidget(id);

    if (items.empty())
    {
        return false;
    }

    index = std::clamp(
        index,
        0,
        static_cast<i32>(items.size()) - 1);

    const std::string ownedId(id);
    ImGui::PushID(ownedId.c_str());

    const ImGuiStyle& style = ImGui::GetStyle();
    constexpr f32 kSegmentGap = 2.0F;
    const f32 totalGap =
        kSegmentGap *
        static_cast<f32>(items.size() - 1U);

    // Segmented controls are used both in full panels and in the permanent
    // Studio shell. Stretching every instance across the entire remaining
    // content region makes a four-item toolbar consume the rest of the row and
    // pushes unrelated controls onto the next line. Size the group from its
    // widest label instead, while still shrinking equally when the parent is
    // genuinely narrower than the natural control width.
    f32 widestLabel = 0.0F;
    for (const std::string_view item : items)
    {
        const ImVec2 text =
            ImGui::CalcTextSize(
                item.data(),
                item.data() + item.size());
        widestLabel = std::max(widestLabel, text.x);
    }

    const f32 naturalSegmentWidth =
        std::max(
            widestLabel + style.FramePadding.x * 2.0F,
            1.0F);
    const f32 available =
        std::max(
            ImGui::GetContentRegionAvail().x,
            static_cast<f32>(items.size()));
    const f32 constrainedSegmentWidth =
        std::max(
            (available - totalGap) /
                static_cast<f32>(items.size()),
            1.0F);
    const f32 width =
        std::min(
            naturalSegmentWidth,
            constrainedSegmentWidth);

    const ImVec4 selectedColor =
        ImGui::GetStyleColorVec4(
            ImGuiCol_ButtonActive);
    const ImVec4 selectedHover =
        ImGui::GetStyleColorVec4(
            ImGuiCol_ButtonHovered);

    bool changed = false;

    ImGui::PushStyleVar(
        ImGuiStyleVar_ItemSpacing,
        ImVec2(kSegmentGap, style.ItemSpacing.y));

    for (std::size_t item = 0U;
         item < items.size();
         ++item)
    {
        if (item != 0U)
        {
            ImGui::SameLine();
        }

        ImGui::PushID(
            static_cast<int>(item));

        const bool selected =
            static_cast<i32>(item) == index;

        if (selected)
        {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                selectedColor);
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered,
                selectedHover);
            ImGui::PushStyleColor(
                ImGuiCol_ButtonActive,
                selectedColor);
        }

        const std::string label(items[item]);
        const bool pressed =
            ImGui::Button(
                label.c_str(),
                ImVec2(width, 0.0F));

        if (selected)
        {
            ImGui::PopStyleColor(3);
        }

        if (pressed && !selected)
        {
            index = static_cast<i32>(item);
            changed = true;
        }

        ImGui::PopID();
    }

    ImGui::PopStyleVar();
    ImGui::PopID();

    return changed;
}
} // namespace orbit::editor_ui
