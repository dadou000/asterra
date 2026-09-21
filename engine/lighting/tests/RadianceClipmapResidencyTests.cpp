#include <orbit/lighting/RadianceClipmapResidency.hpp>

#include <algorithm>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    const RadianceClipmapConfig config{
        .baseCellSizeMeters = 1.0,
        .levelScale = 2.0,
        .levelCount = 2U,
        .cellsPerAxis = 4U
    };

    LightingView view;
    view.frame = frames::FrameId{
        .high = 1U,
        .low = 2U};
    view.body = universe::BodyId{
        .high = 3U,
        .low = 4U};

    RadianceClipmapResidency residency(
        config);

    residency.Reset(
        view,
        {0.0, 0.0, 0.0},
        10U);

    auto initial =
        residency.Stats();

    if (initial.residentCells != 128U ||
        initial.dirtyCells != 128U)
    {
        return 1;
    }

    // Stationary scene converges as update candidates are committed.
    while (true)
    {
        const auto updates =
            residency.BuildUpdateList(
                {0.0, 0.0, 0.0},
                13U);

        if (updates.empty())
        {
            break;
        }

        for (const auto& update :
             updates)
        {
            const bool committed =
                residency.CommitUpdate(
                    update.key,
                    DirectionalIrradianceL1{
                        .l0 = {
                            1.0F,
                            0.5F,
                            0.25F}
                    },
                    16U,
                    10U);

            if (!committed)
            {
                return 2;
            }
        }
    }

    if (residency.Stats().dirtyCells != 0U)
    {
        return 3;
    }

    residency.RequestGlobalRefresh();

    if (residency.Stats().dirtyCells == 0U)
    {
        return 4;
    }

    const auto refreshSnapshot =
        residency.BuildGpuSnapshot(view);

    const auto refreshCandidates =
        residency.BuildUpdateList(
            {0.0, 0.0, 0.0},
            4U);

    if (refreshCandidates.empty())
    {
        return 5;
    }

    const bool hasUsableDirtyCell =
        std::any_of(
            refreshSnapshot.cells.begin(),
            refreshSnapshot.cells.end(),
            [](const auto& cell)
            {
                return cell.irradiance0.w > 0.5F;
            });

    if (!hasUsableDirtyCell)
    {
        return 6;
    }

    const auto oldKey =
        RadianceCellForPoint(
            {0.25, 0.25, 0.25},
            config,
            0U,
            view);

    if (residency.Lookup(
            oldKey,
            10U) == nullptr)
    {
        return 6;
    }

    // One-cell motion reuses most toroidal slots instead of full-clearing.
    const auto scrolled =
        residency.ScrollTo(
            view,
            {1.1, 0.0, 0.0},
            10U,
            0.016F);

    if (scrolled.reusedCells == 0U ||
        scrolled.replacedCells == 0U ||
        scrolled.replacedCells >=
            scrolled.residentCells)
    {
        return 7;
    }

    // Revision change rejects stale cells immediately.
    static_cast<void>(
        residency.ScrollTo(
            view,
            {1.1, 0.0, 0.0},
            11U,
            0.016F));

    if (residency.Lookup(
            oldKey,
            11U) != nullptr ||
        residency.Stats().dirtyCells == 0U)
    {
        return 8;
    }

    const auto updates =
        residency.BuildUpdateList(
            {1.1, 0.0, 0.0},
            8U);

    if (updates.empty())
    {
        return 9;
    }

    // A local invalidation marks nearby cells dirty without affecting the
    // logical key identity.
    const auto targetKey =
        updates.front().key;

    if (!residency.CommitUpdate(
            targetKey,
            {},
            4U,
            11U))
    {
        return 10;
    }

    const auto targetCenter =
        RadianceCellCenterInFrame(
            targetKey,
            config);

    residency.InvalidateSphere(
        targetCenter,
        0.1,
        12U);

    if (residency.Lookup(
            targetKey,
            12U) != nullptr)
    {
        return 11;
    }

    const auto snapshot =
        residency.BuildGpuSnapshot(view);

    if (snapshot.sourceRevision != 12U ||
        snapshot.levels.size() != 2U ||
        snapshot.cells.size() != 128U)
    {
        return 12;
    }

    if (snapshot.levels[0].cellOffset != 0U ||
        snapshot.levels[0].cellCount != 64U ||
        snapshot.levels[1].cellOffset != 64U ||
        snapshot.levels[1].cellCount != 64U)
    {
        return 13;
    }

    const auto invalidIndex =
        snapshot.levels[targetKey.level].cellOffset +
        updates.front().physicalIndex;

    if (invalidIndex >= snapshot.cells.size() ||
        snapshot.cells[invalidIndex].
                irradiance0.w !=
            0.0F)
    {
        return 14;
    }

    return 0;
}
