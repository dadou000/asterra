#include <orbit/editor_ui/EditorUi.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
using orbit::editor_ui::DockAssignment;
using orbit::editor_ui::DockRegion;
using orbit::editor_ui::PanelDefinition;
using orbit::editor_ui::PanelId;

void Check(const bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "Editor dock layout test failed: " << what << "\n";
        std::exit(1);
    }
}

PanelId Id(const orbit::u64 low)
{
    return PanelId{.high = 0x444f434bULL, .low = low};
}

PanelDefinition Panel(
    const orbit::u64 low,
    const DockRegion region,
    const bool pinned = false,
    const orbit::i32 order = 100)
{
    return {
        .id = Id(low),
        .title = "panel",
        .dockToMainViewport = pinned,
        .defaultDock = region,
        .dockOrder = order,
        .draw = [](orbit::editor_ui::PanelContext&) {}
    };
}

DockRegion RegionOf(
    const std::vector<DockAssignment>& assignments,
    const orbit::u64 low)
{
    const auto found = std::ranges::find_if(
        assignments,
        [low](const DockAssignment& assignment)
        {
            return assignment.panel == Id(low);
        });
    Check(found != assignments.end(), "panel is assigned");
    return found->region;
}

void EveryParticipatingPanelGetsAConcreteRegion()
{
    const std::array panels{
        Panel(1, DockRegion::Center),
        Panel(2, DockRegion::Left),
        Panel(3, DockRegion::Right),
        Panel(4, DockRegion::Bottom),
        Panel(5, DockRegion::Auto)};

    const auto assignments =
        orbit::editor_ui::AssignDefaultDock(panels);

    Check(assignments.size() == panels.size(), "all panels assigned");
    Check(RegionOf(assignments, 1) == DockRegion::Center, "center kept");
    Check(RegionOf(assignments, 2) == DockRegion::Left, "left kept");
    Check(RegionOf(assignments, 3) == DockRegion::Right, "right kept");
    Check(RegionOf(assignments, 4) == DockRegion::Bottom, "bottom kept");
    Check(
        RegionOf(assignments, 5) == DockRegion::Right,
        "unknown panels (plugins) fall back to the right tab group");

    for (const DockAssignment& assignment : assignments)
    {
        Check(assignment.region != DockRegion::Auto, "Auto is always resolved");
    }
}

void PinnedShellPanelsAreLeftAlone()
{
    const std::array panels{
        Panel(1, DockRegion::Center, true),
        Panel(2, DockRegion::Left)};

    const auto assignments =
        orbit::editor_ui::AssignDefaultDock(panels);

    Check(assignments.size() == 1, "pinned panel excluded");
    Check(assignments.front().panel == Id(2), "unpinned panel kept");
}

void RegistrationOrderIsPreserved()
{
    const std::array panels{
        Panel(9, DockRegion::Bottom),
        Panel(3, DockRegion::Bottom),
        Panel(7, DockRegion::Bottom)};

    const auto assignments =
        orbit::editor_ui::AssignDefaultDock(panels);

    Check(assignments.size() == 3, "all assigned");
    Check(assignments[0].panel == Id(9), "first stays first");
    Check(assignments[1].panel == Id(3), "second stays second");
    Check(assignments[2].panel == Id(7), "third stays third");
}

void LowerDockOrderComesFirstAndTiesKeepRegistrationOrder()
{
    // Registered secondary-first, as Studio does: the map is registered
    // before the primary viewport, but the viewport must be the first tab.
    const std::array panels{
        Panel(1, DockRegion::Center, false, 10),
        Panel(2, DockRegion::Center, false, 0),
        Panel(3, DockRegion::Center, false, 10),
        Panel(4, DockRegion::Right)};

    const auto assignments =
        orbit::editor_ui::AssignDefaultDock(panels);

    Check(assignments.size() == 4, "all assigned");
    Check(assignments[0].panel == Id(2), "lowest order first");
    Check(assignments[1].panel == Id(1), "tie keeps registration order (1)");
    Check(assignments[2].panel == Id(3), "tie keeps registration order (3)");
    Check(
        assignments[3].panel == Id(4),
        "default-order panels (plugins) come after explicit ones");
}

void SplitPlanOnlySplitsRegionsThatHavePanels()
{
    const std::vector<DockAssignment> onlyCenter{
        {.panel = Id(1), .region = DockRegion::Center}};
    const auto none = orbit::editor_ui::PlanDockSplits(
        onlyCenter,
        orbit::editor_ui::DockLayoutFractions{});

    Check(none.bottomOfRoot == 0.0F, "no bottom split");
    Check(none.leftOfRemainder == 0.0F, "no left split");
    Check(none.rightOfRemainder == 0.0F, "no right split");

    const std::vector<DockAssignment> leftOnly{
        {.panel = Id(1), .region = DockRegion::Center},
        {.panel = Id(2), .region = DockRegion::Left}};
    const auto left = orbit::editor_ui::PlanDockSplits(
        leftOnly,
        orbit::editor_ui::DockLayoutFractions{});

    Check(left.leftOfRemainder > 0.0F, "left split present");
    Check(left.rightOfRemainder == 0.0F, "right split absent");
    Check(left.bottomOfRoot == 0.0F, "bottom split absent");
}

void SplitFractionsAreOfTheWholeDockSpace()
{
    const std::vector<DockAssignment> everything{
        {.panel = Id(1), .region = DockRegion::Center},
        {.panel = Id(2), .region = DockRegion::Left},
        {.panel = Id(3), .region = DockRegion::Right},
        {.panel = Id(4), .region = DockRegion::Bottom}};

    const orbit::editor_ui::DockLayoutFractions fractions{};
    const auto plan =
        orbit::editor_ui::PlanDockSplits(everything, fractions);

    Check(plan.bottomOfRoot == fractions.bottom, "bottom is of the root");
    Check(plan.leftOfRemainder == fractions.left, "left is of the root width");

    // The right split is taken from the width left after the left split, so
    // the width it ends up with must equal the requested whole-space fraction.
    const float rightOfWhole =
        plan.rightOfRemainder * (1.0F - plan.leftOfRemainder);
    Check(
        std::abs(rightOfWhole - fractions.right) < 1.0e-5F,
        "right fraction is of the whole width");

    const float centerWidth =
        1.0F - plan.leftOfRemainder - rightOfWhole;
    Check(centerWidth > 0.4F, "centre keeps most of the width");
}

void OversizedFractionsCannotSwallowTheCentre()
{
    const std::vector<DockAssignment> everything{
        {.panel = Id(1), .region = DockRegion::Center},
        {.panel = Id(2), .region = DockRegion::Left},
        {.panel = Id(3), .region = DockRegion::Right},
        {.panel = Id(4), .region = DockRegion::Bottom}};

    const auto plan = orbit::editor_ui::PlanDockSplits(
        everything,
        {.left = 5.0F, .right = 5.0F, .bottom = 5.0F});

    Check(plan.bottomOfRoot <= 0.40F, "bottom is clamped");
    Check(plan.leftOfRemainder <= 0.40F, "left is clamped");

    const float rightOfWhole =
        plan.rightOfRemainder * (1.0F - plan.leftOfRemainder);
    Check(
        1.0F - plan.leftOfRemainder - rightOfWhole >= 0.2F,
        "centre keeps a usable width even with absurd inputs");

    const auto tiny = orbit::editor_ui::PlanDockSplits(
        everything,
        {.left = 0.0F, .right = 0.0F, .bottom = 0.0F});
    Check(tiny.bottomOfRoot > 0.0F, "zero fraction is raised to a minimum");
    Check(tiny.leftOfRemainder > 0.0F, "zero left is raised to a minimum");
}

void RealWorldFloatingLayoutIsTreatedAsUnset()
{
    // Verbatim shape of the layout a fresh project ended up with: every
    // panel floating at the ImGui default position, empty dock root.
    constexpr std::string_view floating =
        "[Window][WindowOverViewport_11111111]\n"
        "Pos=0,19\nSize=1904,1022\nCollapsed=0\n\n"
        "[Window][Viewport]\nPos=60,60\nSize=1020,76\nCollapsed=0\n\n"
        "[Window][Explorer]\nPos=60,60\nSize=559,165\nCollapsed=0\n\n"
        "[Docking][Data]\n"
        "DockSpace ID=0x08BD597D Window=0x1BBC0F80 Pos=0,19 Size=1904,1022 CentralNode=1\n\n";

    Check(
        !orbit::editor_ui::LayoutTextHasDockedPanels(floating),
        "all-floating layout has no docking");
    Check(
        !orbit::editor_ui::LayoutTextHasDockedPanels(""),
        "empty layout has no docking");
}

void PopulatedLayoutIsKept()
{
    constexpr std::string_view docked =
        "[Window][Viewport]\nPos=60,60\nSize=800,600\nCollapsed=0\n"
        "DockId=0x00000002,0\n\n"
        "[Docking][Data]\n"
        "DockSpace ID=0x08BD597D Window=0x1BBC0F80 Pos=0,19 Size=1904,1022 Split=X\n"
        "  DockNode ID=0x00000001 Parent=0x08BD597D SizeRef=400,1022 Selected=0x1\n"
        "  DockNode ID=0x00000002 Parent=0x08BD597D SizeRef=1500,1022 CentralNode=1\n";

    Check(
        orbit::editor_ui::LayoutTextHasDockedPanels(docked),
        "a docked layout is preserved");
}
} // namespace

int main()
{
    EveryParticipatingPanelGetsAConcreteRegion();
    PinnedShellPanelsAreLeftAlone();
    RegistrationOrderIsPreserved();
    LowerDockOrderComesFirstAndTiesKeepRegistrationOrder();
    SplitPlanOnlySplitsRegionsThatHavePanels();
    SplitFractionsAreOfTheWholeDockSpace();
    OversizedFractionsCannotSwallowTheCentre();
    RealWorldFloatingLayoutIsTreatedAsUnset();
    PopulatedLayoutIsKept();
    return 0;
}
