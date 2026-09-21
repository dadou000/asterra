#include <orbit/celestial_scheduler/CelestialWorkScheduler.hpp>

#include <algorithm>

int main()
{
    using namespace orbit::celestial_scheduler;

    CelestialWorkScheduler scheduler({
        .maxCpuJobsPerFrame = 2U,
        .maxGpuJobsPerFrame = 1U,
        .maxCpuCostUnitsPerFrame = 4U,
        .maxGpuCostUnitsPerFrame = 3U,
        .maxPendingRequests = 4U
    });

    const WorkKey appearance{
        .subjectHigh = 1U,
        .subjectLow = 1U,
        .kind = WorkKind::OrbitalAppearance
    };
    const WorkKey atmosphere{
        .subjectHigh = 1U,
        .subjectLow = 1U,
        .kind = WorkKind::AtmosphereLut
    };
    const WorkKey clouds{
        .subjectHigh = 2U,
        .subjectLow = 2U,
        .kind = WorkKind::CloudField
    };
    const WorkKey impostor{
        .subjectHigh = 3U,
        .subjectLow = 3U,
        .kind = WorkKind::FarImpostor
    };

    scheduler.Enqueue({
        .key = appearance,
        .authorityRevision = 10U,
        .backend = WorkBackend::Cpu,
        .costUnits = 2U,
        .priority = 2,
        .visible = true
    });

    scheduler.Enqueue({
        .key = atmosphere,
        .authorityRevision = 20U,
        .backend = WorkBackend::Gpu,
        .costUnits = 3U,
        .priority = 5,
        .visible = true
    });

    scheduler.Enqueue({
        .key = clouds,
        .authorityRevision = 30U,
        .backend = WorkBackend::Cpu,
        .costUnits = 3U,
        .priority = 1,
        .visible = false
    });

    scheduler.Enqueue({
        .key = impostor,
        .authorityRevision = 40U,
        .backend = WorkBackend::Cpu,
        .costUnits = 2U,
        .priority = 3,
        .visible = true
    });

    const auto first =
        scheduler.BuildFramePlan();

    if (first.size() != 3U)
        return 1;

    const auto has =
        [&](const WorkKey& key)
        {
            return std::any_of(
                first.begin(),
                first.end(),
                [&](const WorkGrant& grant)
                {
                    return grant.key == key;
                });
        };

    if (!has(atmosphere) ||
        !has(impostor) ||
        !has(appearance) ||
        has(clouds))
    {
        return 2;
    }

    const auto stats =
        scheduler.Stats();

    if (stats.cpuJobsGranted != 2U ||
        stats.gpuJobsGranted != 1U ||
        stats.cpuCostGranted != 4U ||
        stats.gpuCostGranted != 3U ||
        stats.pendingRequests != 1U)
    {
        return 3;
    }

    // A newer semantic revision arrives while the old appearance generation
    // is in flight. The old completion must be rejected and never published.
    scheduler.Enqueue({
        .key = appearance,
        .authorityRevision = 11U,
        .backend = WorkBackend::Cpu,
        .costUnits = 2U,
        .priority = 9,
        .visible = true
    });

    if (scheduler.Complete(
            appearance,
            10U))
    {
        return 4;
    }

    if (!scheduler.Complete(
            atmosphere,
            20U) ||
        !scheduler.Complete(
            impostor,
            40U))
    {
        return 5;
    }

    const auto second =
        scheduler.BuildFramePlan();

    if (second.empty() ||
        second.front().key != appearance ||
        second.front().
                authorityRevision !=
            11U)
    {
        return 6;
    }

    // Hard queue cap: low-priority non-visible work must be discarded before
    // visible/high-priority requests.
    for (orbit::u64 i = 0U; i < 8U; ++i)
    {
        scheduler.Enqueue({
            .key = {
                .subjectHigh = 100U + i,
                .subjectLow = 200U + i,
                .kind = WorkKind::CloudField
            },
            .authorityRevision = 1000U + i,
            .backend = WorkBackend::Cpu,
            .costUnits = 1U,
            .priority = -10,
            .visible = false
        });
    }

    const auto capped =
        scheduler.Stats();

    if (capped.pendingRequests >
            scheduler.Budget().
                maxPendingRequests ||
        capped.queueDrops == 0U)
    {
        return 7;
    }

    scheduler.Invalidate(clouds);

    if (scheduler.
            CurrentAuthorityRevision(
                clouds).
            has_value())
    {
        return 8;
    }

    return 0;
}
