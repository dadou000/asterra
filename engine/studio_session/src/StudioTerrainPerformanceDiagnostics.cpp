#include <orbit/studio_session/StudioTerrainPerformanceDiagnostics.hpp>

#include <orbit/core/BuildInfo.hpp>
#include <orbit/studio_session/StudioSession.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace orbit::studio_session
{
namespace
{
[[nodiscard]] std::string CpuBrand()
{
#if defined(_MSC_VER)
    int registers[4]{};
    __cpuid(registers, 0x80000000);

    const unsigned int maximumLeaf =
        static_cast<unsigned int>(
            registers[0]);

    if (maximumLeaf < 0x80000004U)
    {
        return "Unknown CPU";
    }

    std::array<char, 49U> brand{};

    for (unsigned int leaf = 0U;
         leaf < 3U;
         ++leaf)
    {
        __cpuid(
            registers,
            static_cast<int>(
                0x80000002U + leaf));

        std::memcpy(
            brand.data() +
                leaf * 16U,
            registers,
            16U);
    }

    std::string result(
        brand.data());

    const auto first =
        result.find_first_not_of(' ');

    if (first == std::string::npos)
    {
        return "Unknown CPU";
    }

    const auto last =
        result.find_last_not_of(' ');

    return result.substr(
        first,
        last - first + 1U);
#else
    return "Unknown CPU";
#endif
}

[[nodiscard]] f64 HitRate(
    const u64 hits,
    const u64 misses) noexcept
{
    const u64 total =
        hits + misses;

    return total != 0U
        ? static_cast<f64>(hits) *
              100.0 /
              static_cast<f64>(total)
        : 0.0;
}

[[nodiscard]] bool SameObserver(
    const math::Double3& a,
    const math::Double3& b) noexcept
{
    constexpr f64 epsilonMeters =
        1.0e-6;

    return
        math::LengthSquared(a - b) <=
        epsilonMeters * epsilonMeters;
}
} // namespace

void StudioTerrainPerformanceDiagnostics::
RecordFrameCpuSeconds(
    const f64 seconds,
    const bool regenerationActive)
{
    if (!std::isfinite(seconds) ||
        seconds < 0.0)
    {
        return;
    }

    ++frameSamples_;
    lastFrameCpuSeconds_ =
        seconds;
    totalFrameCpuSeconds_ +=
        seconds;
    maximumFrameCpuSeconds_ =
        std::max(
            maximumFrameCpuSeconds_,
            seconds);

    if (!regenerationActive)
    {
        return;
    }

    ++regeneratingFrameSamples_;
    totalRegeneratingFrameCpuSeconds_ +=
        seconds;
    maximumRegeneratingFrameCpuSeconds_ =
        std::max(
            maximumRegeneratingFrameCpuSeconds_,
            seconds);
}

void StudioTerrainPerformanceDiagnostics::
RecordFrameCpuSeconds(
    const f64 seconds,
    StudioSession& session,
    const std::string_view viewportId)
{
    bool active = false;

    const auto runtime =
        session.TerrainRuntime().
            Capture(viewportId);

    if (runtime.has_value())
    {
        if (const auto body =
                session.TerrainPhysicalPages().
                    BodyStatus(
                        runtime->planet.id);
            body.has_value())
        {
            active =
                body->dirtyPages != 0U ||
                body->queuedPages != 0U ||
                body->buildingPages != 0U ||
                body->uploadingPages != 0U ||
                body->stalePages != 0U;
        }

        if (const auto page =
                session.TerrainPhysicalPages().
                    PageStatus(
                        runtime->
                            observerPhysicalPage);
            page.has_value())
        {
            active =
                active ||
                page->awaitingEditReady;
        }
    }

    RecordFrameCpuSeconds(
        seconds,
        active);
}

void StudioTerrainPerformanceDiagnostics::
RecordViewportStreaming(
    const std::string_view viewportId,
    const world::WorldPosition& observer,
    const terrain_gpu::PersistentGpuTerrainCacheStats& cacheStats,
    const StudioTerrainViewportStreamingMetrics& streaming,
    const std::string_view adapterName)
{
    if (viewportId.empty())
    {
        return;
    }

    auto& record =
        viewports_[
            std::string(viewportId)];

    record.adapterName =
        std::string(adapterName);
    record.streaming =
        streaming;

    if (!record.hasObserver ||
        !SameObserver(
            record.observerMeters,
            observer.meters))
    {
        record.hasObserver =
            true;
        record.observerMeters =
            observer.meters;
        record.stationaryFrames =
            0U;
        record.stationaryBaseHits =
            cacheStats.hits;
        record.stationaryBaseMisses =
            cacheStats.misses;
        record.stationaryHits =
            0U;
        record.stationaryMisses =
            0U;
        return;
    }

    ++record.stationaryFrames;

    record.stationaryHits =
        cacheStats.hits >=
                record.stationaryBaseHits
            ? cacheStats.hits -
                  record.stationaryBaseHits
            : 0U;

    record.stationaryMisses =
        cacheStats.misses >=
                record.stationaryBaseMisses
            ? cacheStats.misses -
                  record.stationaryBaseMisses
            : 0U;
}

StudioTerrainPerformanceSnapshot
StudioTerrainPerformanceDiagnostics::Capture(
    StudioSession& session,
    const std::string_view viewportId) const
{
    StudioTerrainPerformanceSnapshot result{
        .viewportId =
            std::string(viewportId),
        .cpuName =
            CpuBrand(),
        .buildConfiguration =
            std::string(
                build::BuildConfiguration),
        .engineVersion =
            std::string(
                build::Version),
        .sourceCommit =
            std::string(
                build::GitCommit),
        .frameSamples =
            frameSamples_,
        .lastFrameCpuMs =
            lastFrameCpuSeconds_ *
            1'000.0,
        .averageFrameCpuMs =
            frameSamples_ != 0U
                ? totalFrameCpuSeconds_ *
                      1'000.0 /
                      static_cast<f64>(
                          frameSamples_)
                : 0.0,
        .maximumFrameCpuMs =
            maximumFrameCpuSeconds_ *
            1'000.0,
        .regeneratingFrameSamples =
            regeneratingFrameSamples_,
        .averageRegeneratingFrameCpuMs =
            regeneratingFrameSamples_ != 0U
                ? totalRegeneratingFrameCpuSeconds_ *
                      1'000.0 /
                      static_cast<f64>(
                          regeneratingFrameSamples_)
                : 0.0,
        .maximumRegeneratingFrameCpuMs =
            maximumRegeneratingFrameCpuSeconds_ *
            1'000.0,
        .m30Reference =
            M30Reference()
    };

    const auto record =
        viewports_.find(
            viewportId);

    if (record != viewports_.end())
    {
        result.gpuName =
            record->second.adapterName;
        result.streaming =
            record->second.streaming;
        result.stationaryFrames =
            record->second.stationaryFrames;
        result.stationaryCacheHits =
            record->second.stationaryHits;
        result.stationaryCacheMisses =
            record->second.stationaryMisses;
        result.stationaryCacheHitRatePercent =
            HitRate(
                result.stationaryCacheHits,
                result.stationaryCacheMisses);
    }

    const auto runtime =
        session.TerrainRuntime().
            Capture(viewportId);

    if (!runtime.has_value())
    {
        return result;
    }

    result.hasTerrainRuntime =
        true;
    result.cacheStats =
        runtime->cacheStats;
    result.cacheHitRatePercent =
        HitRate(
            result.cacheStats.hits,
            result.cacheStats.misses);

    const auto bodyStatus =
        session.TerrainPhysicalPages().
            BodyStatus(
                runtime->planet.id);

    if (bodyStatus.has_value())
    {
        result.residentTrackedPages =
            bodyStatus->pages;
        result.dirtyPages =
            bodyStatus->dirtyPages;
        result.queuedPages =
            bodyStatus->queuedPages;
        result.buildingPages =
            bodyStatus->buildingPages;
        result.uploadingPages =
            bodyStatus->uploadingPages;
        result.stalePages =
            bodyStatus->stalePages;
        result.failedPages =
            bodyStatus->failedPages;
        result.staleRejected =
            bodyStatus->staleRejected;
        result.peakOutstandingPages =
            bodyStatus->
                peakOutstandingPages;

        result.outstandingPages =
            result.dirtyPages +
            result.queuedPages +
            result.buildingPages +
            result.uploadingPages +
            result.stalePages;

        result.regenerationActive =
            result.outstandingPages != 0U;
    }

    const auto pageStatus =
        session.TerrainPhysicalPages().
            PageStatus(
                runtime->
                    observerPhysicalPage);

    if (pageStatus.has_value())
    {
        result.selectedPageState =
            TerrainRebuildStateName(
                pageStatus->state);

        result.selectedEditToReadyMs =
            pageStatus->
                    lastEditToReadySeconds >=
                    0.0
                ? pageStatus->
                      lastEditToReadySeconds *
                      1'000.0
                : -1.0;

        result.maximumEditToReadyMs =
            pageStatus->
                    maximumEditToReadySeconds >=
                    0.0
                ? pageStatus->
                      maximumEditToReadySeconds *
                      1'000.0
                : -1.0;

        result.selectedAwaitingReady =
            pageStatus->
                awaitingEditReady;

        result.regenerationActive =
            result.regenerationActive ||
            result.selectedAwaitingReady;
    }

    return result;
}

void StudioTerrainPerformanceDiagnostics::
Reset() noexcept
{
    frameSamples_ = 0U;
    lastFrameCpuSeconds_ = 0.0;
    totalFrameCpuSeconds_ = 0.0;
    maximumFrameCpuSeconds_ = 0.0;

    regeneratingFrameSamples_ = 0U;
    totalRegeneratingFrameCpuSeconds_ = 0.0;
    maximumRegeneratingFrameCpuSeconds_ = 0.0;

    viewports_.clear();
}

StudioTerrainM30Reference
StudioTerrainPerformanceDiagnostics::
M30Reference()
{
    return {
        .capturedUtc =
            "2026-09-20T13:16:13Z",
        .adapter =
            "NVIDIA GeForce RTX 4090",
        .buildConfiguration =
            "Release",
        .sourceCommit =
            "fdca9966a608c0e75b7cadc121ed10843ca62983",
        .resolution =
            129U,
        .pageGenerationGpuMs =
            10.638688,
        .peakTransientBytes =
            1'065'024U,
        .persistentPageBytes =
            332'820U,
        .cacheHitRatePercent =
            99.609375,
        .hydraulicIterationGpuMs =
            0.023552,
        .aeolianIterationGpuMs =
            0.033792,
        .drainageBuildGpuMs =
            3.606112,
        .scatterGenerationGpuMs =
            0.019136
    };
}
} // namespace orbit::studio_session
