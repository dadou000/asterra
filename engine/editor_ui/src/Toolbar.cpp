#include <orbit/editor_ui/EditorUi.hpp>
#include <imgui.h>
#include <algorithm>
#include <numbers>
#include <string>

namespace orbit::editor_ui
{
namespace
{
ElementCategory CategoryForIcon(const ToolbarIcon icon)
{
    switch (icon)
    {
    case ToolbarIcon::World:
    case ToolbarIcon::Box:
    case ToolbarIcon::Sphere:
    case ToolbarIcon::Frame:
    case ToolbarIcon::Camera:
        return ElementCategory::Meshes;
    case ToolbarIcon::Renderer:
    case ToolbarIcon::GlobalIllumination:
    case ToolbarIcon::AntiAliasing:
    case ToolbarIcon::PointLight:
    case ToolbarIcon::SpotLight:
        return ElementCategory::Lighting;
    case ToolbarIcon::Atmosphere:
    case ToolbarIcon::Clouds:
    case ToolbarIcon::Ocean:
    case ToolbarIcon::Rings:
    case ToolbarIcon::Aurora:
    case ToolbarIcon::Planet:
    case ToolbarIcon::Surface:
        return ElementCategory::Celestial;
    case ToolbarIcon::Procedural:
        return ElementCategory::Procedurals;
    case ToolbarIcon::Vfx:
        return ElementCategory::Vfx;
    case ToolbarIcon::Sfx:
        return ElementCategory::Sfx;
    case ToolbarIcon::Decal:
        return ElementCategory::Shading;
    default:
        return ElementCategory::Neutral;
    }
}

ImU32 SemanticIconColor(const ToolbarIcon icon, const ImU32 fallback)
{
    const ElementCategory category = CategoryForIcon(icon);
    if (category == ElementCategory::Neutral)
        return fallback;
    const math::Float4 categoryColor = ElementCategoryColor(category);
    ImVec4 tint(categoryColor.x, categoryColor.y,
        categoryColor.z, categoryColor.w);
    tint.w = ((fallback >> IM_COL32_A_SHIFT) & 0xff) / 255.0F;
    return ImGui::ColorConvertFloat4ToU32(tint);
}

void DrawToolIcon(ImDrawList& draw, ToolbarIcon icon, ImVec2 origin, f32 scale, ImU32 color)
{
    const auto p = [=](f32 x, f32 y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
    const f32 stroke = 1.6F * scale;
    const auto line = [&](f32 x, f32 y, f32 a, f32 b) { draw.AddLine(p(x,y), p(a,b), color, stroke); };
    const auto circle = [&](f32 x, f32 y, f32 r) { draw.AddCircle(p(x,y), r * scale, color, 24, stroke); };
    const auto rect = [&](f32 x, f32 y, f32 a, f32 b) { draw.AddRect(p(x,y), p(a,b), color, scale, 0, stroke); };
    switch (icon)
    {
    case ToolbarIcon::Select:
        line(4,2,4,16); line(4,2,15,12); line(15,12,9,12); line(9,12,12,18); line(4,16,9,12); break;
    case ToolbarIcon::Move:
        line(10,2,10,18); line(2,10,18,10);
        line(7,5,10,2); line(13,5,10,2); line(7,15,10,18); line(13,15,10,18);
        line(5,7,2,10); line(5,13,2,10); line(15,7,18,10); line(15,13,18,10); break;
    case ToolbarIcon::Rotate:
        draw.PathArcTo(p(10,10),7*scale,-2.5F,2.2F,24); draw.PathStroke(color,0,stroke);
        line(4,3,4,8); line(4,8,9,8); break;
    case ToolbarIcon::Scale:
        rect(2,12,8,18); line(8,12,17,3); line(11,3,17,3); line(17,3,17,9); break;
    case ToolbarIcon::World:
        circle(10,10,8); draw.AddEllipse(p(10,10),ImVec2(4*scale,8*scale),color,0,24,stroke); line(2,10,18,10); break;
    case ToolbarIcon::Local:
        line(5,15,5,3); line(5,15,17,15); line(5,15,13,7); line(3,5,5,3); line(7,5,5,3); line(15,13,17,15); break;
    case ToolbarIcon::Snap:
        line(4,3,4,12); line(16,3,16,12); draw.PathArcTo(p(10,12),6*scale,0,std::numbers::pi_v<f32>,20); draw.PathStroke(color,0,stroke);
        line(2,6,6,6); line(14,6,18,6); break;
    case ToolbarIcon::PointLight:
        circle(10,10,4); line(10,1,10,3); line(10,17,10,19); line(1,10,3,10); line(17,10,19,10);
        line(3,3,5,5); line(15,15,17,17); line(3,17,5,15); line(15,5,17,3); break;
    case ToolbarIcon::SpotLight:
        line(3,3,10,3); line(3,3,7,8); line(10,3,7,8); line(7,8,3,17); line(7,8,17,17); line(3,17,17,17); break;
    case ToolbarIcon::Box:
        line(10,2,18,6); line(18,6,18,14); line(18,14,10,18); line(10,18,2,14); line(2,14,2,6); line(2,6,10,2);
        line(2,6,10,10); line(18,6,10,10); line(10,10,10,18); break;
    case ToolbarIcon::Sphere:
        circle(10,10,8); draw.AddEllipse(p(10,10),ImVec2(8*scale,3*scale),color,0,24,stroke); break;
    case ToolbarIcon::Frame:
        line(2,7,2,2); line(2,2,7,2); line(13,2,18,2); line(18,2,18,7);
        line(2,13,2,18); line(2,18,7,18); line(13,18,18,18); line(18,18,18,13); circle(10,10,3); break;
    case ToolbarIcon::Duplicate:
        rect(7,7,18,18); line(13,7,13,2); line(13,2,2,2); line(2,2,2,13); line(2,13,7,13); break;
    case ToolbarIcon::Delete:
        line(3,5,17,5); line(7,2,13,2); line(5,5,6,18); line(6,18,14,18); line(14,18,15,5); line(8,8,8,15); line(12,8,12,15); break;
    case ToolbarIcon::Undo: case ToolbarIcon::Redo:
    {
        const bool redo = icon == ToolbarIcon::Redo;
        const auto q = [&](f32 x) { return redo ? 20-x : x; };
        line(q(3),7,q(12),7); line(q(3),7,q(7),3); line(q(3),7,q(7),11);
        draw.PathArcTo(p(q(12),12),5*scale,redo ? std::numbers::pi_v<f32>*0.5F : -std::numbers::pi_v<f32>*0.5F,
            redo ? std::numbers::pi_v<f32>*1.5F : std::numbers::pi_v<f32>*0.5F,20); draw.PathStroke(color,0,stroke); break;
    }
    case ToolbarIcon::Properties:
        line(2,5,18,5); line(2,10,18,10); line(2,15,18,15); rect(5,3,8,7); rect(12,8,15,12); rect(6,13,9,17); break;
    case ToolbarIcon::Atmosphere: circle(10,10,8); circle(10,10,5); break;
    case ToolbarIcon::Clouds:
        circle(7,10,4); circle(12,7,4); circle(16,11,3); line(4,14,17,14); break;
    case ToolbarIcon::Ocean:
        for (int y=5;y<=15;y+=5) { line(2,static_cast<f32>(y),6,static_cast<f32>(y-2)); line(6,static_cast<f32>(y-2),12,static_cast<f32>(y+1)); line(12,static_cast<f32>(y+1),18,static_cast<f32>(y-1)); } break;
    case ToolbarIcon::Rings:
        circle(10,10,5); draw.AddEllipse(p(10,10),ImVec2(9*scale,3*scale),color,-0.4F,32,stroke); break;
    case ToolbarIcon::Planet:
        circle(10,10,6); draw.AddEllipse(p(10,10),ImVec2(9*scale,3*scale),color,-0.4F,32,stroke); break;
    case ToolbarIcon::Aurora:
        line(2,15,5,5); line(5,5,8,12); line(8,12,12,3); line(12,3,15,13); line(15,13,18,7); break;
    case ToolbarIcon::Surface:
        line(2,17,7,6); line(7,6,11,12); line(11,12,14,8); line(14,8,18,17); line(2,17,18,17); break;
    case ToolbarIcon::Procedural:
        line(2,15,6,5); line(6,5,10,15); line(10,15,14,5); line(14,5,18,15);
        circle(6,5,2); circle(14,5,2); circle(10,15,2); break;
    case ToolbarIcon::Vfx:
        line(10,1,12,7); line(12,7,18,10); line(18,10,12,12); line(12,12,10,19);
        line(10,19,8,12); line(8,12,2,10); line(2,10,8,7); line(8,7,10,1); break;
    case ToolbarIcon::Sfx:
        line(2,8,6,8); line(6,8,11,3); line(11,3,11,17); line(11,17,6,12); line(6,12,2,12); line(2,12,2,8);
        draw.PathArcTo(p(11,10),6*scale,-0.8F,0.8F,12); draw.PathStroke(color,0,stroke); break;
    case ToolbarIcon::Asset:
        line(2,7,10,7); line(10,7,13,10); line(13,10,18,10); line(18,10,17,18);
        line(17,18,3,18); line(3,18,2,7); line(2,7,4,3); line(4,3,9,3); line(9,3,11,5); break;
    case ToolbarIcon::Decal:
        rect(3,3,17,17); line(12,3,17,8); line(12,3,12,8); line(12,8,17,8);
        circle(7,12,1.5F); break;
    case ToolbarIcon::Camera:
        rect(2,6,18,17); line(6,6,8,3); line(8,3,13,3); line(13,3,15,6);
        circle(10,11.5F,3.5F); break;
    case ToolbarIcon::Renderer:
        circle(10,10,7.5F); circle(10,10,2.2F);
        line(10,1,10,4); line(10,16,10,19);
        line(1,10,4,10); line(16,10,19,10); break;
    case ToolbarIcon::GlobalIllumination:
        circle(10,10,3.5F); line(10,1,10,4); line(10,16,10,19);
        line(1,10,4,10); line(16,10,19,10);
        line(3.5F,3.5F,5.5F,5.5F); line(14.5F,14.5F,16.5F,16.5F);
        line(3.5F,16.5F,5.5F,14.5F); line(14.5F,5.5F,16.5F,3.5F); break;
    case ToolbarIcon::AntiAliasing:
        line(2,16,7,11); line(7,11,12,8); line(12,8,18,3);
        circle(7,11,1.2F); circle(12,8,1.2F); break;
      case ToolbarIcon::Visibility:
         draw.AddEllipse(p(10,10),ImVec2(8*scale,5*scale),color,0,24,stroke);
         circle(10,10,2.5F); break;
     case ToolbarIcon::Layers:
         draw.AddLine(p(10,3),p(18,7),color,stroke);
         draw.AddLine(p(18,7),p(10,11),color,stroke);
         draw.AddLine(p(10,11),p(2,7),color,stroke);
         draw.AddLine(p(2,7),p(10,3),color,stroke);
         draw.AddLine(p(3,11),p(10,15),color,stroke);
         draw.AddLine(p(10,15),p(17,11),color,stroke);
         draw.AddLine(p(3,15),p(10,19),color,stroke);
         draw.AddLine(p(10,19),p(17,15),color,stroke);
         break;
    case ToolbarIcon::More:
        for (f32 x : {4.0F,10.0F,16.0F}) draw.AddCircleFilled(p(x,10),1.5F*scale,color,12); break;
    }
}
}

math::Float4 ElementCategoryColor(const ElementCategory category) noexcept
{
    switch (category)
    {
    case ElementCategory::Meshes: return {0.431F, 0.659F, 0.996F, 1.0F}; // #6EA8FE
    case ElementCategory::Procedurals: return {0.694F, 0.592F, 0.988F, 1.0F}; // #B197FC
    case ElementCategory::Lighting: return {0.965F, 0.784F, 0.373F, 1.0F}; // #F6C85F
    case ElementCategory::Simulation: return {0.333F, 0.776F, 0.702F, 1.0F}; // #55C6B3
    case ElementCategory::Vfx: return {0.910F, 0.475F, 0.976F, 1.0F}; // #E879F9
    case ElementCategory::Sfx: return {0.949F, 0.545F, 0.510F, 1.0F}; // #F28B82
    case ElementCategory::Shading: return {0.545F, 0.608F, 1.0F, 1.0F}; // #8B9BFF
    case ElementCategory::Celestial: return {0.361F, 0.784F, 0.910F, 1.0F}; // #5CC8E8
    case ElementCategory::Plugins: return {0.651F, 0.690F, 0.765F, 1.0F}; // #A6B0C3
    case ElementCategory::Planning: return {0.475F, 0.788F, 0.541F, 1.0F}; // #79C98A
    case ElementCategory::Neutral: return {0.78F, 0.80F, 0.84F, 1.0F};
    }
    return {0.78F, 0.80F, 0.84F, 1.0F};
}

void PanelContext::DecorateLastRow(
    const ToolbarIcon icon,
    const bool muted,
    const f32 inset)
{
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    if (maximum.y <= minimum.y)
        return;

    const f32 scale = CurrentUiScale();
    const f32 iconScale = 0.75F * scale;
    const f32 size = 20.0F * iconScale;
    const ImVec2 origin(
        minimum.x + inset * scale,
        minimum.y + (maximum.y - minimum.y - size) * 0.5F);
    DrawToolIcon(
        *ImGui::GetWindowDrawList(),
        icon,
        origin,
        iconScale,
        muted
            ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
            : SemanticIconColor(icon, ImGui::GetColorU32(ImGuiCol_Text)));
}

TreeItemInteraction PanelContext::TreeItemWithIcon(
    const std::string_view label,
    const bool selected,
    const ToolbarIcon icon,
    const f32 iconInset)
{
    return TreeItemWithIcon(label, selected, icon, iconInset, 0.0F);
}

TreeItemInteraction PanelContext::TreeItemWithIcon(
    const std::string_view label,
    const bool selected,
    const ToolbarIcon icon,
    const f32 iconInset,
    const f32 extraVerticalPadding)
{
    const f32 scale = CurrentUiScale();
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    const auto visible = label.substr(0, label.find("##"));
    const auto hiddenId = label.find("##");
    const std::string nativeLabel = hiddenId == std::string_view::npos
        ? std::string(label)
        : std::string("##") + std::string(label.substr(hiddenId + 2));
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(padding.x, padding.y + (2.0F + extraVerticalPadding) * scale));
    const auto item = TreeItem(nativeLabel, selected);
    ImGui::PopStyleVar();
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    auto& draw = *ImGui::GetWindowDrawList();
    const ImU32 color = SemanticIconColor(
        icon, ImGui::GetColorU32(ImGuiCol_Text));
    const f32 iconScale = scale * 0.8F;
    DrawToolIcon(draw, icon,
        ImVec2(rowMin.x + iconInset * scale,
            rowMin.y + (rowMax.y - rowMin.y - 20.0F * iconScale) * 0.5F),
        iconScale, color);
    const f32 fontSize = ImGui::GetFontSize();
    const f32 textInset = iconInset + 28.0F;
    draw.AddText(ImGui::GetFont(), fontSize,
        ImVec2(rowMin.x + textInset * scale,
            rowMin.y + (rowMax.y - rowMin.y - fontSize) * 0.5F),
        color, visible.data(), visible.data() + visible.size());
    return item;
}

bool PanelContext::ToolbarButton(std::string_view label, ToolbarIcon icon,
    bool selected, bool enabled, bool compact, ToolbarStyle style)
{
    TraceWidget(label);
    const f32 scale = CurrentUiScale();
    const bool modifier = style == ToolbarStyle::Modifier || style == ToolbarStyle::ModifierMenu;
    const bool menu = style == ToolbarStyle::Menu || style == ToolbarStyle::ModifierMenu;
    const std::string owned(label);
    const auto visible = label.substr(0,label.find("##"));
    const f32 fontSize = ImGui::GetFontSize() * (modifier ? 0.86F : 1.0F);
    const f32 labelWidth = ImGui::CalcTextSize(visible.data(),visible.data()+visible.size()).x * (modifier ? 0.86F : 1.0F);
    const f32 height = 32*scale;
    const f32 width = (compact ? (modifier ? 26 : 32)*scale : labelWidth + (modifier ? 30 : 42)*scale) + (menu ? 12*scale : 0);
    const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,5*scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0);
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(accent.x,accent.y,accent.z,!modifier && selected ? 0.22F : 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(accent.x,accent.y,accent.z,modifier ? 0 : 0.14F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(accent.x,accent.y,accent.z,modifier ? 0 : 0.3F));
    ImGui::BeginDisabled(!enabled);
    ImGui::PushID(owned.c_str());
    const bool pressed = ImGui::Button("##tool",ImVec2(width,height));
    ImGui::PopID();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImVec2 visualA(a.x,a.y+(modifier ? 4*scale : 0));
    const ImVec2 visualB(b.x,b.y-(modifier ? 4*scale : 0));
    auto& draw = *ImGui::GetWindowDrawList();
    if (modifier && (selected || ImGui::IsItemHovered()))
        draw.AddRectFilled(visualA,visualB,ImGui::GetColorU32(ImVec4(accent.x,accent.y,accent.z,selected ? 0.16F : 0.10F)),4*scale);
    ImVec4 foreground = ImGui::GetStyleColorVec4(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    if (!enabled) foreground.w *= 0.65F;
    else if (modifier && !selected) foreground.w *= 0.75F;
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(foreground);
    const f32 iconScale = scale * (modifier ? 0.75F : 1.0F);
    DrawToolIcon(draw,icon,ImVec2(a.x+(modifier ? 5 : compact ? 6 : 9)*scale,
        a.y+(height-20*iconScale)*0.5F),iconScale,
        enabled ? SemanticIconColor(icon, color) : color);
    if (!compact) draw.AddText(ImGui::GetFont(),fontSize,
        ImVec2(a.x+(modifier ? 25 : 34)*scale,a.y+(height-fontSize)*0.5F),color,visible.data(),visible.data()+visible.size());
    if (menu)
    {
        const f32 x = b.x-9*scale, y = a.y+height*0.5F;
        draw.AddLine(ImVec2(x-3*scale,y-2*scale),ImVec2(x,y+scale),color,1.4F*scale);
        draw.AddLine(ImVec2(x,y+scale),ImVec2(x+3*scale,y-2*scale),color,1.4F*scale);
    }
    if (selected && !modifier) draw.AddRect(visualA,visualB,ImGui::GetColorU32(ImVec4(accent.x,accent.y,accent.z,0.45F)),5*scale,0,scale);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%.*s",static_cast<int>(visible.size()),visible.data());
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    return pressed;
}

bool PanelContext::ToolbarChoices(std::string_view id, std::span<const ToolbarChoice> items, i32& index, bool compact, ToolbarStyle style)
{
    TraceWidget(id);
    const std::string owned(id);
    ImGui::PushID(owned.c_str());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(3*CurrentUiScale(),0));
    bool changed = false;
    for (std::size_t i=0; i<items.size(); ++i)
    {
        if (i) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(i));
        if (ToolbarButton(items[i].label,items[i].icon,index == static_cast<i32>(i),true,compact,style))
        { changed = index != static_cast<i32>(i); index = static_cast<i32>(i); }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

bool PanelContext::SelectableWithIcon(
    const std::string_view label,
    const bool selected,
    const ToolbarIcon icon)
{
    return SelectableWithIcon(label, selected, icon, 0.0F);
}

bool PanelContext::SelectableWithIcon(
    const std::string_view label,
    const bool selected,
    const ToolbarIcon icon,
    const f32 extraVerticalPadding)
{
    TraceWidget(label);
    const auto visible = label.substr(0, label.find("##"));
    const auto hiddenId = label.find("##");
    const std::string nativeLabel = hiddenId == std::string_view::npos
        ? std::string(label)
        : std::string("##") + std::string(label.substr(hiddenId + 2));
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        ImVec2(padding.x,
            padding.y + extraVerticalPadding * CurrentUiScale()));
    const bool pressed = Selectable(nativeLabel, selected);
    ImGui::PopStyleVar();
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    const f32 scale = CurrentUiScale();
    const f32 iconScale = 0.8F * scale;
    const f32 rowHeight = rowMax.y - rowMin.y;
    auto& draw = *ImGui::GetWindowDrawList();
    DrawToolIcon(draw, icon,
        ImVec2(rowMin.x + 7.0F * scale,
            rowMin.y + (rowHeight - 20.0F * iconScale) * 0.5F),
        iconScale, SemanticIconColor(icon, ImGui::GetColorU32(ImGuiCol_Text)));
    const f32 fontSize = ImGui::GetFontSize();
    draw.AddText(ImGui::GetFont(), fontSize,
        ImVec2(rowMin.x + 34.0F * scale,
            rowMin.y + (rowHeight - fontSize) * 0.5F),
        ImGui::GetColorU32(ImGuiCol_Text),
        visible.data(), visible.data() + visible.size());
    return pressed;
}

void PanelContext::ToolbarDivider()
{
    const f32 scale = CurrentUiScale();
    const f32 gap = (ImGui::GetWindowWidth() < 1900.0F * scale ? 3.0F : 12.0F) * scale;
    ImGui::SameLine(0,gap);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(scale,32*scale));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x,p.y+7*scale),ImVec2(p.x,p.y+25*scale),
        ImGui::GetColorU32(ImGuiCol_Separator),scale);
    ImGui::SameLine(0,gap);
}
} // namespace orbit::editor_ui
