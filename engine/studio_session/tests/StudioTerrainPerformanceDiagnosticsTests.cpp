#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_session/StudioTerrainPerformanceDiagnostics.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr
            << "Studio terrain performance diagnostics test failed.\n";
        std::exit(1);
    }
}
} // namespace

int main()
{
    const auto root =
        std::filesystem::temp_directory_path() /
        ("orbit-studio-m16-diagnostics-" +
         orbit::documents::ProjectId::Random().
             ToString());

    std::filesystem::remove_all(root);

    {
        auto project =
            orbit::documents::ProjectDocument::Create(
                root,
                "M16 Diagnostics");

        orbit::studio_session::StudioSession
            session(project);

        auto& diagnostics =
            session.TerrainPerformance();

        diagnostics.RecordFrameCpuSeconds(
            0.004,
            false);
        diagnostics.RecordFrameCpuSeconds(
            0.008,
            true);

        const orbit::world::WorldPosition observer{
            .meters = {
                1.0,
                2.0,
                3.0
            }
        };

        orbit::terrain_gpu::
            PersistentGpuTerrainCacheStats
            firstCache{
                .hits = 10U,
                .misses = 2U,
                .generations = 4U,
                .insertions = 4U,
                .evictions = 0U,
                .residentPages = 3U,
                .residentBytes = 4096U
            };

        diagnostics.RecordViewportStreaming(
            "studio.primary",
            observer,
            firstCache,
            {
                .generatedSamplesLastUpdate =
                    256U,
                .refreshedRegionsLastUpdate =
                    2U,
                .levelsTouchedLastUpdate =
                    1U,
                .uploadedBytesLastFrame =
                    8192U,
                .drawCallsLastFrame =
                    3U,
                .cumulativeGeneratedSamples =
                    1024U,
                .cumulativeUploadedBytes =
                    32768U,
                .submittedBatches =
                    8U,
                .committedBatches =
                    7U,
                .supersededBatches =
                    1U,
                .revisionInvalidations =
                    2U,
                .staleRevisionBatches =
                    1U,
                .coverageTierChanges =
                    0U,
                .rebaseCount =
                    0U,
                .adaptiveCoverageTier =
                    0U,
                .activeBaseSpacingMeters =
                    20.0,
                .activeOuterHalfExtentMeters =
                    1000.0,
                .updatePending =
                    false
            },
            "M16 Test GPU");

        auto secondCache =
            firstCache;

        secondCache.hits = 15U;

        diagnostics.RecordViewportStreaming(
            "studio.primary",
            observer,
            secondCache,
            {
                .uploadedBytesLastFrame =
                    4096U,
                .drawCallsLastFrame =
                    2U,
                .cumulativeGeneratedSamples =
                    1024U,
                .cumulativeUploadedBytes =
                    36864U,
                .submittedBatches =
                    8U,
                .committedBatches =
                    8U,
                .activeBaseSpacingMeters =
                    20.0,
                .activeOuterHalfExtentMeters =
                    1000.0
            },
            "M16 Test GPU");

        const auto snapshot =
            diagnostics.Capture(
                session,
                "studio.primary");

        Check(
            !snapshot.cpuName.empty());
        Check(
            snapshot.gpuName ==
            "M16 Test GPU");
        Check(
            !snapshot.buildConfiguration.empty());
        Check(
            !snapshot.engineVersion.empty());
        Check(
            !snapshot.sourceCommit.empty());

        Check(
            snapshot.frameSamples ==
            2U);
        Check(
            snapshot.lastFrameCpuMs >=
            7.99 &&
            snapshot.lastFrameCpuMs <=
            8.01);
        Check(
            snapshot.averageFrameCpuMs >=
            5.99 &&
            snapshot.averageFrameCpuMs <=
            6.01);
        Check(
            snapshot.regeneratingFrameSamples ==
            1U);
        Check(
            snapshot.averageRegeneratingFrameCpuMs >=
            7.99);

        Check(
            snapshot.stationaryFrames ==
            1U);
        Check(
            snapshot.stationaryCacheHits ==
            5U);
        Check(
            snapshot.stationaryCacheMisses ==
            0U);
        Check(
            snapshot.stationaryCacheHitRatePercent ==
            100.0);

        Check(
            snapshot.streaming.drawCallsLastFrame ==
            2U);
        Check(
            snapshot.streaming.uploadedBytesLastFrame ==
            4096U);
        Check(
            snapshot.streaming.committedBatches ==
            8U);

        Check(
            snapshot.m30Reference.adapter ==
            "NVIDIA GeForce RTX 4090");
        Check(
            snapshot.m30Reference.resolution ==
            129U);
        Check(
            snapshot.m30Reference.pageGenerationGpuMs >
            0.0);
        Check(
            snapshot.m30Reference.peakTransientBytes >
            0U);
    }

    std::filesystem::remove_all(root);

    std::cout
        << "Orbit V0.0.5 M16 terrain performance diagnostics tests passed.\n";
    return 0;
}
