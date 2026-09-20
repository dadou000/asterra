#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/studio_session/StudioTerrainRebuildScheduler.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/world/WorldPosition.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace orbit::studio_session
{
class StudioSession;

struct StudioTerrainViewportStreamingMetrics
{
    u64 generatedSamplesLastUpdate{0U};
    u32 refreshedRegionsLastUpdate{0U};
    u32 levelsTouchedLastUpdate{0U};

    u64 uploadedBytesLastFrame{0U};
    u32 drawCallsLastFrame{0U};

    u64 cumulativeGeneratedSamples{0U};
    u64 cumulativeUploadedBytes{0U};

    u64 submittedBatches{0U};
    u64 committedBatches{0U};
    u64 supersededBatches{0U};
    u64 revisionInvalidations{0U};
    u64 staleRevisionBatches{0U};
    u64 coverageTierChanges{0U};
    u64 rebaseCount{0U};

    u32 adaptiveCoverageTier{0U};
    f64 activeBaseSpacingMeters{0.0};
    f64 activeOuterHalfExtentMeters{0.0};
    bool updatePending{false};
};

struct StudioTerrainM30Reference
{
    std::string capturedUtc;
    std::string adapter;
    std::string buildConfiguration;
    std::string sourceCommit;
    u32 resolution{0U};

    f64 pageGenerationGpuMs{0.0};
    u64 peakTransientBytes{0U};
    u64 persistentPageBytes{0U};
    f64 cacheHitRatePercent{0.0};
    f64 hydraulicIterationGpuMs{0.0};
    f64 aeolianIterationGpuMs{0.0};
    f64 drainageBuildGpuMs{0.0};
    f64 scatterGenerationGpuMs{0.0};
};

struct StudioTerrainPerformanceSnapshot
{
    std::string viewportId;

    std::string cpuName;
    std::string gpuName;
    std::string buildConfiguration;
    std::string engineVersion;
    std::string sourceCommit;

    u64 frameSamples{0U};
    f64 lastFrameCpuMs{0.0};
    f64 averageFrameCpuMs{0.0};
    f64 maximumFrameCpuMs{0.0};

    u64 regeneratingFrameSamples{0U};
    f64 averageRegeneratingFrameCpuMs{0.0};
    f64 maximumRegeneratingFrameCpuMs{0.0};

    bool hasTerrainRuntime{false};
    bool regenerationActive{false};

    u32 residentTrackedPages{0U};
    u32 outstandingPages{0U};
    u32 peakOutstandingPages{0U};
    u32 dirtyPages{0U};
    u32 queuedPages{0U};
    u32 buildingPages{0U};
    u32 uploadingPages{0U};
    u32 stalePages{0U};
    u32 failedPages{0U};
    u64 staleRejected{0U};

    std::string selectedPageState;
    f64 selectedEditToReadyMs{-1.0};
    f64 maximumEditToReadyMs{-1.0};
    bool selectedAwaitingReady{false};

    terrain_gpu::PersistentGpuTerrainCacheStats cacheStats{};
    f64 cacheHitRatePercent{0.0};

    u32 stationaryFrames{0U};
    u64 stationaryCacheHits{0U};
    u64 stationaryCacheMisses{0U};
    f64 stationaryCacheHitRatePercent{0.0};

    StudioTerrainViewportStreamingMetrics streaming{};
    StudioTerrainM30Reference m30Reference{};
};

// M16 presentation diagnostics only. This class never mutates terrain
// authority, scheduling policy, cache contents or render residency.
class StudioTerrainPerformanceDiagnostics
{
public:
    void RecordFrameCpuSeconds(
        f64 seconds,
        bool regenerationActive);

    void RecordViewportStreaming(
        std::string_view viewportId,
        const world::WorldPosition& observer,
        const terrain_gpu::PersistentGpuTerrainCacheStats& cacheStats,
        const StudioTerrainViewportStreamingMetrics& streaming,
        std::string_view adapterName);

    [[nodiscard]] StudioTerrainPerformanceSnapshot Capture(
        StudioSession& session,
        std::string_view viewportId) const;

    void Reset() noexcept;

    [[nodiscard]] static StudioTerrainM30Reference
    M30Reference();

private:
    struct ViewportRecord
    {
        std::string adapterName;
        StudioTerrainViewportStreamingMetrics streaming{};

        bool hasObserver{false};
        math::Double3 observerMeters{};

        u32 stationaryFrames{0U};
        u64 stationaryBaseHits{0U};
        u64 stationaryBaseMisses{0U};
        u64 stationaryHits{0U};
        u64 stationaryMisses{0U};
    };

    u64 frameSamples_{0U};
    f64 lastFrameCpuSeconds_{0.0};
    f64 totalFrameCpuSeconds_{0.0};
    f64 maximumFrameCpuSeconds_{0.0};

    u64 regeneratingFrameSamples_{0U};
    f64 totalRegeneratingFrameCpuSeconds_{0.0};
    f64 maximumRegeneratingFrameCpuSeconds_{0.0};

    std::map<
        std::string,
        ViewportRecord,
        std::less<>>
        viewports_;
};
} // namespace orbit::studio_session
