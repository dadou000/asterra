#include <orbit/studio_ui/StudioTerrainDiagnosticOverlayGeometry.hpp>

#include <orbit/terrain_debug/TerrainDebugPageData.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr
            << "Studio terrain diagnostic overlay geometry test failed.\n";
        std::exit(1);
    }
}

class FlatTerrain final
    : public orbit::terrain::TerrainSource
{
public:
    [[nodiscard]] orbit::terrain::TerrainSample
    Sample(
        const orbit::terrain::TerrainQuery&)
        const noexcept override
    {
        return {
            .elevationMeters = 100.0
        };
    }

    [[nodiscard]] orbit::u64
    Revision() const noexcept override
    {
        return 1U;
    }
};

orbit::studio_session::
    StudioTerrainViewportRuntimeSnapshot
MakeRuntime()
{
    orbit::studio_session::
        StudioTerrainViewportRuntimeSnapshot
        runtime{
            .viewportId = "test",
            .body = {
                .high = 3U,
                .low = 4U
            },
            .planet = {
                .radiusMeters = 6'000'000.0,
                .id = {
                    .high = 5U,
                    .low = 6U
                }
            }
        };

    runtime.observerPhysicalPage = {
        .planet = runtime.planet.id,
        .tile = {
            .face =
                orbit::world::CubeFace::
                    NegativeZ,
            .level = 8U,
            .x = 127U,
            .y = 127U
        }
    };

    runtime.layout.levels.push_back({
        .index = 0U,
        .gridResolution = 65U,
        .sampleSpacingMeters = 20.0,
        .terrainFootprintMeters = 20.0,
        .innerHoleHalfExtentMeters = 0.0,
        .outerHalfExtentMeters = 640.0,
        .morphStartHalfExtentMeters = 500.0,
        .morphEndHalfExtentMeters = 620.0
    });

    const auto direction =
        orbit::world::CubeToUnitDirection(
            orbit::world::TileCenter(
                runtime.
                    observerPhysicalPage.
                    tile));

    runtime.motion.levels.push_back({
        .levelIndex = 0U,
        .centerDirection = direction,
        .surfaceFrame =
            orbit::world::MakeSurfaceFrame(
                direction),
        .centerOffsetMeters = {}
    });

    return runtime;
}

orbit::studio_ui::StudioTerrainDiagnosticPage
MakePage(
    const orbit::studio_session::
        StudioTerrainViewportRuntimeSnapshot& runtime)
{
    auto debug =
        std::make_shared<
            orbit::terrain_debug::
                TerrainDebugPageData>(
                    orbit::terrain_debug::
                        TerrainDebugPageStamp{
                            .address =
                                runtime.
                                    observerPhysicalPage,
                            .physicalLod = 3U,
                            .revisions = {
                                .geology = 1U,
                                .climate = 1U,
                                .authoring = 1U,
                                .biome = 1U,
                                .water = 1U,
                                .processes = 1U
                            },
                            .cacheResident = true,
                            .invalidationRevision = 2U
                        },
                    5U,
                    5U);

    const std::vector<orbit::f32>
        biome{
            0.0F, 0.0F, 0.2F, 0.4F, 0.6F,
            0.0F, 0.2F, 0.4F, 0.6F, 0.8F,
            0.2F, 0.4F, 0.6F, 0.8F, 1.0F,
            0.4F, 0.6F, 0.8F, 1.0F, 1.0F,
            0.6F, 0.8F, 1.0F, 1.0F, 1.0F
        };

    const std::vector<orbit::f32>
        process{
            0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
            0.0F,-1.0F,-0.5F, 0.5F, 0.0F,
            0.0F,-0.5F, 0.0F, 0.5F, 0.0F,
            0.0F, 0.5F, 1.0F, 0.5F, 0.0F,
            0.0F, 0.0F, 0.0F, 0.0F, 0.0F
        };

    std::vector<
        orbit::terrain_debug::
            TerrainDebugVector2>
        drainage(
            25U,
            {});

    drainage[12U] = {
        .x = 2.0F,
        .y = 1.0F
    };

    debug->SetScalar(
        orbit::terrain_debug::
            TerrainDebugField::
                BiomeWeights,
        biome);

    debug->SetScalar(
        orbit::terrain_debug::
            TerrainDebugField::
                ErosionDeposition,
        process);

    debug->SetVector(
        orbit::terrain_debug::
            TerrainDebugField::
                Drainage,
        drainage);

    auto snapshot =
        std::make_shared<
            orbit::studio_session::
                StudioTerrainPhysicalPageSnapshot>();

    snapshot->address =
        runtime.observerPhysicalPage;
    snapshot->physicalLod = 3U;
    snapshot->cacheResident = true;
    snapshot->debugPage =
        std::move(debug);

    return {
        .status = {
            .address =
                runtime.
                    observerPhysicalPage,
            .state =
                orbit::studio_session::
                    TerrainRebuildState::
                        Dirty,
            .dirtyProducts = 1U
        },
        .snapshot =
            std::move(snapshot)
    };
}
} // namespace

int main()
{
    FlatTerrain terrain;
    const auto runtime =
        MakeRuntime();

    orbit::render_view::CameraState
        camera{};

    camera.localPositionMeters = {
        0.0,
        0.0,
        -6'010'000.0
    };

    const auto page =
        MakePage(runtime);

    const std::vector<
        orbit::studio_ui::
            StudioTerrainDiagnosticPage>
        pages{
            page
        };

    {
        const auto lines =
            orbit::studio_ui::
                BuildTerrainDiagnosticOverlayLines(
                    {},
                    runtime,
                    terrain,
                    pages,
                    camera);

        Check(lines.empty());
    }

    {
        orbit::studio_ui::
            StudioTerrainDiagnosticOverlayOptions
            options{};

        options.dirtyPageBounds = true;

        const auto lines =
            orbit::studio_ui::
                BuildTerrainDiagnosticOverlayLines(
                    options,
                    runtime,
                    terrain,
                    pages,
                    camera);

        Check(lines.size() >= 48U);
    }

    {
        orbit::studio_ui::
            StudioTerrainDiagnosticOverlayOptions
            options{};

        options.clipmapRings = true;

        const auto lines =
            orbit::studio_ui::
                BuildTerrainDiagnosticOverlayLines(
                    options,
                    runtime,
                    terrain,
                    pages,
                    camera);

        Check(lines.size() >= 64U);
    }

    {
        orbit::studio_ui::
            StudioTerrainDiagnosticOverlayOptions
            options{};

        options.physicalLod = true;

        const auto lines =
            orbit::studio_ui::
                BuildTerrainDiagnosticOverlayLines(
                    options,
                    runtime,
                    terrain,
                    pages,
                    camera);

        Check(!lines.empty());
    }

    {
        orbit::studio_ui::
            StudioTerrainDiagnosticOverlayOptions
            options{};

        options.biomeWeights = true;

        const auto lines =
            orbit::studio_ui::
                BuildTerrainDiagnosticOverlayLines(
                    options,
                    runtime,
                    terrain,
                    pages,
                    camera);

        Check(!lines.empty());
    }

    {
        orbit::studio_ui::
            StudioTerrainDiagnosticOverlayOptions
            options{};

        options.processMasks = true;

        const auto lines =
            orbit::studio_ui::
                BuildTerrainDiagnosticOverlayLines(
                    options,
                    runtime,
                    terrain,
                    pages,
                    camera);

        Check(!lines.empty());
    }

    {
        orbit::studio_ui::
            StudioTerrainDiagnosticOverlayOptions
            options{};

        options.drainageVectors = true;

        const auto lines =
            orbit::studio_ui::
                BuildTerrainDiagnosticOverlayLines(
                    options,
                    runtime,
                    terrain,
                    pages,
                    camera);

        Check(!lines.empty());
    }

    return 0;
}
