#include <orbit/studio_ui/StudioViewportCamera.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "Studio viewport camera test failed.\n";
        std::exit(1);
    }
}

orbit::studio_session::ViewportTargetState Target(
    const orbit::studio_session::ViewportMode mode,
    const orbit::u64 universeGeneration)
{
    orbit::studio_session::ViewportTargetState result{
        .id = "test",
        .mode = mode,
        .followActiveBody = true
    };

    result.target = orbit::editor_session::ActiveBodyTarget{
        .semanticObject = {
            .high = 1,
            .low = 2
        },
        .body = {
            .high = 3,
            .low = 4
        },
        .frame = {
            .high = 5,
            .low = 6
        },
        .name = "Asterra",
        .referenceRadiusMeters = 6'000'000.0,
        .sessionGeneration = 7,
        .universeGeneration = universeGeneration,
        .sourceRevision = 8
    };
    return result;
}
} // namespace

int main()
{
    constexpr orbit::u64 generation = 42;

    {
        const auto camera =
            orbit::studio_ui::ComposeViewportCamera(
                Target(
                    orbit::studio_session::ViewportMode::Perspective,
                    generation),
                generation);
        Check(camera.has_value());
        Check(camera->frame.high == 5);
        Check(camera->frame.low == 6);
        Check(camera->localPositionMeters.x == 0.0);
        Check(camera->localPositionMeters.y == 0.0);
        Check(camera->localPositionMeters.z == -19'200'000.0);
        Check(camera->forward.z == 1.0F);
        Check(camera->nearPlaneMeters == 6.0F);
        Check(camera->farPlaneMeters == 72'000'000.0F);
    }

    {
        const auto camera =
            orbit::studio_ui::ComposeViewportCamera(
                Target(
                    orbit::studio_session::ViewportMode::BodyMap,
                    generation),
                generation);
        Check(camera.has_value());
        Check(camera->localPositionMeters.y == 19'200'000.0);
        Check(camera->forward.y == -1.0F);
        Check(camera->up.z == 1.0F);
    }

    {
        const auto camera =
            orbit::studio_ui::ComposeViewportCamera(
                Target(
                    orbit::studio_session::ViewportMode::Debug,
                    generation),
                generation);
        Check(camera.has_value());
        Check(camera->localPositionMeters.x == -14'400'000.0);
        Check(camera->localPositionMeters.y == 10'800'000.0);
        Check(camera->localPositionMeters.z == -14'400'000.0);
        Check(camera->forward.x > 0.6F);
        Check(camera->forward.y < -0.4F);
    }

    {
        const auto target =
            Target(
                orbit::studio_session::ViewportMode::Perspective,
                generation);

        orbit::studio_session::
            StudioTerrainViewportRuntimeSnapshot terrain{
                .viewportId = "test",
                .worldGeneration = 7,
                .universeGeneration = generation,
                .runtimeGeneration = 1,
                .semanticBody = {
                    .high = 1,
                    .low = 2
                },
                .body = {
                    .high = 3,
                    .low = 4
                },
                .terrainObject = {
                    .high = 9,
                    .low = 10
                },
                .planet = {
                    .radiusMeters = 6'000'000.0,
                    .id = {
                        .high = 3,
                        .low = 4
                    }
                },
                .observer = {
                    .meters = {
                        6'010'000.0,
                        0.0,
                        0.0
                    }
                }
            };

        const auto camera =
            orbit::studio_ui::
                ComposeTerrainViewportCamera(
                    target,
                    terrain);

        Check(
            camera.frame.high == 5 &&
            camera.frame.low == 6);
        Check(
            camera.localPositionMeters ==
                terrain.observer.meters);
        Check(
            camera.nearPlaneMeters ==
                20.0F);
        Check(
            camera.farPlaneMeters >
                500'000.0F);
        Check(
            camera.farPlaneMeters <
                1'000'000.0F);

        const auto observerDirection =
            orbit::math::Normalize(
                terrain.observer.meters);

        const orbit::math::Double3
            cameraForward{
                camera.forward.x,
                camera.forward.y,
                camera.forward.z
            };

        const orbit::math::Double3
            cameraUp{
                camera.up.x,
                camera.up.y,
                camera.up.z
            };

        Check(
            orbit::math::Dot(
                cameraForward,
                observerDirection) <
            0.0);

        Check(
            orbit::math::Dot(
                cameraUp,
                observerDirection) >
            0.999);
    }

    {
        const auto target =
            Target(
                orbit::studio_session::ViewportMode::BodyMap,
                generation);
        const auto camera =
            orbit::studio_ui::ComposeViewportCamera(
                target,
                generation);
        Check(camera.has_value());

        const auto page =
            orbit::studio_ui::PhysicalPageAtViewportPoint(
                target,
                *camera,
                640,
                480,
                0.5F,
                0.5F,
                8);

        Check(page.has_value());
        Check(page->address.planet.high == 3);
        Check(page->address.planet.low == 4);
        Check(page->address.tile.level == 8);
        Check(
            page->address.tile ==
            orbit::world::TileForDirection(
                page->surfaceDirection,
                8));
        Check(
            page->address.tile.face ==
            orbit::world::CubeFace::PositiveY);
    }

    {
        const auto target =
            Target(
                orbit::studio_session::ViewportMode::Debug,
                generation);
        const auto camera =
            orbit::studio_ui::ComposeViewportCamera(
                target,
                generation);
        Check(camera.has_value());
        Check(
            !orbit::studio_ui::PhysicalPageAtViewportPoint(
                 target,
                 *camera,
                 640,
                 480,
                 -0.1F,
                 0.5F,
                 8).
                 has_value());
    }

    {
        orbit::studio_session::ViewportTargetState blank{
            .id = "blank",
            .mode = orbit::studio_session::ViewportMode::Perspective,
            .followActiveBody = true
        };
        Check(
            !orbit::studio_ui::ComposeViewportCamera(
                 blank,
                 generation).
                 has_value());
    }

    {
        bool staleRejected = false;
        try
        {
            static_cast<void>(
                orbit::studio_ui::ComposeViewportCamera(
                    Target(
                        orbit::studio_session::ViewportMode::Perspective,
                        generation - 1U),
                    generation));
        }
        catch (const std::logic_error&)
        {
            staleRejected = true;
        }
        Check(staleRejected);
    }

    return 0;
}
