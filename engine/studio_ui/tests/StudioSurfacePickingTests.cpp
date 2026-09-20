#include <orbit/studio_ui/StudioSurfacePicking.hpp>

#include <orbit/math/Vector.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr
            << "Studio surface picking test failed.\n";
        std::exit(1);
    }
}

class TestTerrainSource final
    : public orbit::terrain::TerrainSource
{
public:
    [[nodiscard]] orbit::terrain::TerrainSample
    Sample(
        const orbit::terrain::TerrainQuery& query)
        const noexcept override
    {
        const auto direction =
            orbit::math::Normalize(
                query.unitDirection);

        return {
            .elevationMeters =
                1'000.0 +
                direction.y * 200.0,
            .coarseElevationMeters =
                1'000.0,
            .standingWaterDepthMeters =
                5.0
        };
    }

    [[nodiscard]] orbit::u64
    Revision() const noexcept override
    {
        return 77U;
    }
};

orbit::studio_session::ViewportTargetState
Target(
    const orbit::studio_session::ViewportMode mode)
{
    orbit::studio_session::ViewportTargetState result{
        .id = "studio.test",
        .mode = mode,
        .followActiveBody = true
    };

    result.target =
        orbit::editor_session::ActiveBodyTarget{
            .semanticObject = {
                .high = 1U,
                .low = 2U
            },
            .body = {
                .high = 3U,
                .low = 4U
            },
            .frame = {
                .high = 5U,
                .low = 6U
            },
            .name = "Asterra",
            .referenceRadiusMeters =
                6'000'000.0,
            .sessionGeneration = 7U,
            .universeGeneration = 42U,
            .sourceRevision = 8U
        };

    return result;
}

orbit::studio_session::
StudioTerrainViewportRuntimeSnapshot
Runtime()
{
    return {
        .viewportId = "studio.test",
        .worldGeneration = 7U,
        .universeGeneration = 42U,
        .runtimeGeneration = 9U,
        .semanticBody = {
            .high = 1U,
            .low = 2U
        },
        .body = {
            .high = 3U,
            .low = 4U
        },
        .terrainObject = {
            .high = 10U,
            .low = 11U
        },
        .planet = {
            .radiusMeters =
                6'000'000.0,
            .id = {
                .high = 3U,
                .low = 4U
            }
        },
        .observer = {
            .meters = {
                0.0,
                0.0,
                -18'000'000.0
            }
        },
        .physicalPageLevel = 8U,
        .terrainSourceRevision = 77U
    };
}

orbit::render_view::CameraState CameraForDirection(
    const orbit::math::Double3& outward)
{
    const auto direction =
        orbit::math::Normalize(
            outward);

    orbit::render_view::CameraState camera{};
    camera.localPositionMeters =
        direction *
        18'000'000.0;
    camera.forward = {
        static_cast<orbit::f32>(
            -direction.x),
        static_cast<orbit::f32>(
            -direction.y),
        static_cast<orbit::f32>(
            -direction.z)
    };
    camera.up = {
        0.0F,
        1.0F,
        0.0F
    };
    camera.verticalFovRadians =
        1.0F;
    camera.nearPlaneMeters =
        1.0F;
    camera.farPlaneMeters =
        30'000'000.0F;
    return camera;
}

bool Near(
    const orbit::f64 a,
    const orbit::f64 b,
    const orbit::f64 epsilon)
{
    return std::abs(a - b) <= epsilon;
}
} // namespace

int main()
{
    TestTerrainSource source;
    const auto runtime = Runtime();

    const auto camera =
        CameraForDirection({
            0.0,
            0.0,
            -1.0
        });

    const auto perspective =
        orbit::studio_ui::
            PickStudioTerrainSurface(
                Target(
                    orbit::studio_session::
                        ViewportMode::Perspective),
                camera,
                640U,
                480U,
                0.5F,
                0.5F,
                runtime,
                source);

    Check(perspective.has_value());
    Check(
        perspective->body ==
        runtime.body);
    Check(
        perspective->semanticBody ==
        runtime.semanticBody);
    Check(
        perspective->surface.planet ==
        runtime.planet.id);
    Check(
        Near(
            perspective->
                surface.unitDirection.x,
            0.0,
            1.0e-8));
    Check(
        Near(
            perspective->
                surface.unitDirection.y,
            0.0,
            1.0e-8));
    Check(
        perspective->
            surface.unitDirection.z <
        -0.999999);
    Check(
        Near(
            perspective->
                physicalElevationMeters,
            1'000.0,
            1.0e-3));
    Check(
        Near(
            perspective->
                renderedElevationMeters,
            1'005.0,
            1.0e-3));
    Check(
        Near(
            perspective->
                surface.radialOffsetMeters,
            perspective->
                physicalElevationMeters,
            1.0e-9));
    Check(
        perspective->
            physicalPage.has_value());
    Check(
        perspective->
            physicalPage->tile ==
        orbit::world::TileForDirection(
            perspective->
                surface.unitDirection,
            8U));
    Check(
        perspective->
            physicalLod.has_value() &&
        *perspective->physicalLod ==
            8U);
    Check(
        perspective->
            provenance.width ==
        640U);
    Check(
        perspective->
            provenance.height ==
        480U);
    Check(
        perspective->
            provenance.terrainSourceRevision ==
        77U);

    // Same normalized ray + same aspect ratio keeps canonical identity through
    // render-target resize.
    const auto resized =
        orbit::studio_ui::
            PickStudioTerrainSurface(
                Target(
                    orbit::studio_session::
                        ViewportMode::Perspective),
                camera,
                1280U,
                960U,
                0.5F,
                0.5F,
                runtime,
                source);

    Check(resized.has_value());
    Check(
        orbit::math::Length(
            resized->
                surface.unitDirection -
            perspective->
                surface.unitDirection) <
        1.0e-10);
    Check(
        Near(
            resized->
                physicalElevationMeters,
            perspective->
                physicalElevationMeters,
            1.0e-6));

    // Viewport mode is presentation state. If the exact same ray is used in
    // another mode, canonical picked identity remains unchanged.
    const auto mapMode =
        orbit::studio_ui::
            PickStudioTerrainSurface(
                Target(
                    orbit::studio_session::
                        ViewportMode::BodyMap),
                camera,
                640U,
                480U,
                0.5F,
                0.5F,
                runtime,
                source);

    Check(mapMode.has_value());
    Check(
        orbit::math::Length(
            mapMode->
                surface.unitDirection -
            perspective->
                surface.unitDirection) <
        1.0e-10);

    // Opposite sides of a cube-face seam remain continuous in canonical
    // planet space even though tile projection may switch faces.
    const auto seamA =
        orbit::studio_ui::
            PickStudioTerrainSurface(
                Target(
                    orbit::studio_session::
                        ViewportMode::Perspective),
                CameraForDirection({
                    1.0,
                    0.0,
                    1.0 - 1.0e-7
                }),
                640U,
                480U,
                0.5F,
                0.5F,
                runtime,
                source);

    const auto seamB =
        orbit::studio_ui::
            PickStudioTerrainSurface(
                Target(
                    orbit::studio_session::
                        ViewportMode::Perspective),
                CameraForDirection({
                    1.0 - 1.0e-7,
                    0.0,
                    1.0
                }),
                640U,
                480U,
                0.5F,
                0.5F,
                runtime,
                source);

    Check(seamA.has_value());
    Check(seamB.has_value());
    Check(
        orbit::math::Length(
            seamA->
                surface.unitDirection -
            seamB->
                surface.unitDirection) <
        1.0e-6);
    Check(
        seamA->
            physicalPage.has_value());
    Check(
        seamB->
            physicalPage.has_value());

    Check(
        !orbit::studio_ui::
            PickStudioTerrainSurface(
                Target(
                    orbit::studio_session::
                        ViewportMode::Perspective),
                camera,
                640U,
                480U,
                -0.01F,
                0.5F,
                runtime,
                source).
            has_value());

    return 0;
}
