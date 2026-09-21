#include <orbit/lighting/EmissiveInvalidation.hpp>

#include <array>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    EmissiveInvalidationTracker tracker;

    const RuntimeEmissiveSurface liveSurface{
        .geometry = {
            .frame = frames::FrameId{
                .high = 1U,
                .low = 2U},
            .body = universe::BodyId{
                .high = 3U,
                .low = 4U},
            .stableId = 55U,
            .contentRevision = 9U,
            .originInFrameMeters =
                {-1.0, -0.5, 2.0},
            .axisUInFrameMeters =
                {2.0, 0.0, 0.0},
            .axisVInFrameMeters =
                {0.0, 1.0, 0.0}
        },
        .contentRevision = 12U,
        .width = 1U,
        .height = 1U,
        .giRadiance = {
            {1.0F, 2.0F, 3.0F}
        }
    };

    const auto liveState =
        BuildDynamicEmissiveSourceState(
            liveSurface,
            6.0);

    if (liveState.stableId != 55U ||
        liveState.contentRevision != 12U ||
        liveState.centerInFrameMeters.z != 2.0 ||
        liveState.sourceRadiusMeters <= 1.0 ||
        liveState.influenceRangeMeters != 6.0)
    {
        return 1;
    }

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
        return 2;
    }

    events =
        tracker.Update(
            std::span(
                &screen,
                1U));

    if (!events.empty())
    {
        return 3;
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
        return 4;
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
        return 5;
    }

    events = tracker.Update(
        std::span<const DynamicEmissiveSourceState>{});

    if (events.size() != 1U ||
        events.front().reason !=
            EmissiveInvalidationReason::Removed ||
        tracker.SourceCount() != 0U)
    {
        return 6;
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
                return 7;
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
        return 8;
    }

    const auto prioritized =
        residency.BuildUpdateList(
            {100.0, 100.0, 100.0},
            1U);

    if (prioritized.empty())
    {
        return 9;
    }

    return 0;
}
