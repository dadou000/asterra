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

std::vector<PanelMenuEntry> BuildPanelMenu(
    const std::span<const PanelDefinition> panels,
    const std::span<const u8> panelOpen)
{
    std::vector<PanelMenuEntry> entries;
    entries.reserve(
        panels.size());

    for (std::size_t index = 0;
         index < panels.size();
         ++index)
    {
        const PanelDefinition& panel =
            panels[index];

        if (panel.dockToMainViewport)
        {
            continue;
        }

        entries.push_back({
            .panel = panel.id,
            .title = panel.title,
            .region = Resolve(
                panel.defaultDock),
            .open =
                index < panelOpen.size() &&
                panelOpen[index] != 0U
        });
    }

    // Region order matches the on-screen arrangement (centre first, then the
    // side and bottom groups); within a region the dock order decides. The sort
    // is stable so registration order breaks ties, exactly as in the layout.
    const auto orderOf =
        [&panels](const PanelMenuEntry& entry)
        {
            const auto found =
                std::ranges::find_if(
                    panels,
                    [&entry](const PanelDefinition& panel)
                    {
                        return panel.id == entry.panel;
                    });
            return found == panels.end()
                ? 100
                : found->dockOrder;
        };

    std::ranges::stable_sort(
        entries,
        [&orderOf](
            const PanelMenuEntry& left,
            const PanelMenuEntry& right)
        {
            if (left.region != right.region)
            {
                return static_cast<int>(left.region) <
                    static_cast<int>(right.region);
            }

            return orderOf(left) <
                orderOf(right);
        });

    return entries;
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
