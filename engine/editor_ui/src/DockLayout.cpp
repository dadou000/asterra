#include <orbit/editor_ui/EditorUi.hpp>

#include <algorithm>

namespace orbit::editor_ui
{
namespace
{
constexpr f32 kMinFraction = 0.05F;
constexpr f32 kMaxFraction = 0.40F;

[[nodiscard]] DockRegion Resolve(
    const DockRegion region) noexcept
{
    return region == DockRegion::Auto
        ? DockRegion::Right
        : region;
}

[[nodiscard]] f32 Clamped(
    const f32 fraction) noexcept
{
    return std::clamp(
        fraction,
        kMinFraction,
        kMaxFraction);
}
} // namespace

std::vector<DockAssignment> AssignDefaultDock(
    const std::span<const PanelDefinition> panels)
{
    std::vector<const PanelDefinition*> participating;
    participating.reserve(
        panels.size());

    for (const PanelDefinition& panel :
         panels)
    {
        // Pinned shell panels manage their own placement every frame.
        if (!panel.dockToMainViewport)
        {
            participating.push_back(
                &panel);
        }
    }

    std::ranges::stable_sort(
        participating,
        [](const PanelDefinition* left,
           const PanelDefinition* right)
        {
            return left->dockOrder <
                right->dockOrder;
        });

    std::vector<DockAssignment> assignments;
    assignments.reserve(
        participating.size());

    for (const PanelDefinition* panel :
         participating)
    {
        assignments.push_back({
            .panel = panel->id,
            .region = Resolve(
                panel->defaultDock)
        });
    }

    return assignments;
}

DockSplitPlan PlanDockSplits(
    const std::span<const DockAssignment> assignments,
    const DockLayoutFractions& fractions)
{
    const auto has =
        [assignments](const DockRegion region)
        {
            return std::ranges::any_of(
                assignments,
                [region](const DockAssignment& assignment)
                {
                    return assignment.region == region;
                });
        };

    DockSplitPlan plan;

    if (has(DockRegion::Bottom))
    {
        plan.bottomOfRoot =
            Clamped(fractions.bottom);
    }

    f32 remainingWidth = 1.0F;

    if (has(DockRegion::Left))
    {
        plan.leftOfRemainder =
            Clamped(fractions.left);
        remainingWidth -=
            plan.leftOfRemainder;
    }

    if (has(DockRegion::Right))
    {
        // Relative to the width left after the left split, so that the
        // requested fraction is of the whole dock space.
        plan.rightOfRemainder =
            std::clamp(
                Clamped(fractions.right) /
                    remainingWidth,
                kMinFraction,
                0.60F);
    }

    return plan;
}

bool LayoutTextHasDockedPanels(
    const std::string_view layoutText) noexcept
{
    // A populated layout stores split/tab nodes ("DockNode ID=...") and
    // gives each docked window a "DockId=...". A bare "DockSpace ID=..." line
    // is only the empty root.
    return layoutText.find(
               "DockNode ") !=
               std::string_view::npos ||
        layoutText.find(
            "DockId=") !=
            std::string_view::npos;
}
} // namespace orbit::editor_ui
