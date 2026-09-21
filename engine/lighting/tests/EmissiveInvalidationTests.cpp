#include <orbit/lighting/EmissiveInvalidation.hpp>

#include <array>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    EmissiveInvalidationTracker tracker;

    const DynamicEmissiveSourceState screen{
        .stableId = 10U,
        .contentRevision = 1U,
        .centerInFrameMeters =
            {0.0, 0.0, 0.0},
        .sourceRadiusMeters = 1.0,
        .influenceRangeMeters = 8.0
    };

    auto events =
        tracker.Update(
            std::span(
                &screen,
                1U));

    if (events.size() != 1U ||
        events.front().reason !=
            EmissiveInvalidationReason::Added ||
        events.front().radiusMeters != 8.0)
    {
        return 1;
    }

    events =
        tracker.Update(
            std::span(
                &screen,
                1U));

    if (!events.empty())
    {
        return 2;
    }

    auto animated = screen;
    animated.contentRevision = 2U;

    events =
        tracker.Update(
            std::span(
                &animated,
                1U));

    if (events.size() != 1U ||
        events.front().reason !=
            EmissiveInvalidationReason::Changed)
    {
        return 3;
    }

    auto moved = animated;
    moved.contentRevision = 3U;
    moved.centerInFrameMeters =
        {4.0, 0.0, 0.0};

    events =
        tracker.Update(
            std::span(
                &moved,
                1U));

    if (events.size() != 1U ||
        events.front().radiusMeters <=
            moved.influenceRangeMeters)
    {
        return 4;
    }

    events = tracker.Update({});

    if (events.size() != 1U ||
        events.front().reason !=
            EmissiveInvalidationReason::Removed ||
        tracker.SourceCount() != 0U)
    {
        return 5;
    }

    LightingView view;
    view.frame = frames::FrameId{
        .high = 1U,
        .low = 2U};
    view.body = universe::BodyId{
        .high = 3U,
        .low = 4U};

    RadianceClipmapResidency residency({
        .baseCellSizeMeters = 1.0,
        .levelScale = 2.0,
        .levelCount = 1U,
        .cellsPerAxis = 8U
    });

    residency.Reset(
        view,
        {0.0, 0.0, 0.0},
        1U);

    while (true)
    {
        const auto updates =
            residency.BuildUpdateList(
                {0.0, 0.0, 0.0},
                64U);

        if (updates.empty())
        {
            break;
        }

        for (const auto& update :
             updates)
        {
            if (!residency.CommitUpdate(
                    update.key,
                    {},
                    1U,
                    1U))
            {
                return 6;
            }
        }
    }

    const EmissiveInvalidationEvent local{
        .stableId = 10U,
        .reason =
            EmissiveInvalidationReason::Changed,
        .centerInFrameMeters =
            {0.0, 0.0, 0.0},
        .radiusMeters = 1.2
    };

    ApplyEmissiveInvalidations(
        residency,
        std::span(
            &local,
            1U),
        2U,
        8.0F);

    const auto stats =
        residency.Stats();

    if (stats.dirtyCells == 0U ||
        stats.dirtyCells >=
            stats.residentCells)
    {
        return 7;
    }

    const auto prioritized =
        residency.BuildUpdateList(
            {100.0, 100.0, 100.0},
            1U);

    if (prioritized.empty())
    {
        return 8;
    }

    return 0;
}
