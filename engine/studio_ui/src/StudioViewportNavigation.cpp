#include <orbit/studio_ui/StudioViewportNavigation.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::studio_ui
{
namespace
{
constexpr f64 kMinimumObserverAboveReferenceMeters = 0.01;

[[nodiscard]] math::Double3 ObserverDirection(
    const world::WorldPosition& observer) noexcept
{
    const math::Double3 direction =
        math::Normalize(
            observer.meters);

    return math::LengthSquared(direction) >
            1.0e-20
        ? direction
        : math::Double3{1.0, 0.0, 0.0};
}

[[nodiscard]] f64 SampleElevation(
    const terrain::TerrainSource& source,
    const world::PlanetDefinition& planet,
    const math::Double3& unitDirection,
    const f64 footprintMeters) noexcept
{
    const terrain::TerrainSample sample =
        source.Sample({
            .unitDirection =
                math::Normalize(
                    unitDirection),
            .footprintMeters =
                std::clamp(
                    footprintMeters,
                    1.0,
                    10'000.0),
            .planet =
                planet.id,
            .radialOffsetMeters =
                0.0
        });

    return std::isfinite(
               sample.elevationMeters)
        ? sample.elevationMeters
        : 0.0;
}

[[nodiscard]] f64 ObserverAltitudeAboveTerrain(
    const world::PlanetDefinition& planet,
    const terrain::TerrainSource& source,
    const world::WorldPosition& observer) noexcept
{
    const f64 observerRadius =
        math::Length(
            observer.meters);

    const f64 referenceAltitude =
        std::max(
            observerRadius -
                planet.radiusMeters,
            0.0);

    const f64 footprint =
        std::max(
            referenceAltitude * 0.02,
            1.0);

    const f64 elevation =
        SampleElevation(
            source,
            planet,
            ObserverDirection(observer),
            footprint);

    return
        observerRadius -
        planet.radiusMeters -
        elevation;
}

[[nodiscard]] world::WorldPosition ObserverAtDirection(
    const world::PlanetDefinition& planet,
    const terrain::TerrainSource& source,
    const math::Double3& unitDirection,
    const f64 heightAboveTerrainMeters) noexcept
{
    const math::Double3 direction =
        math::Normalize(
            unitDirection);

    const math::Double3 safeDirection =
        math::LengthSquared(direction) >
                1.0e-20
            ? direction
            : math::Double3{1.0, 0.0, 0.0};

    const f64 elevation =
        SampleElevation(
            source,
            planet,
            safeDirection,
            std::max(
                heightAboveTerrainMeters *
                    0.02,
                1.0));

    const f64 radius =
        std::max(
            planet.radiusMeters +
                elevation +
                heightAboveTerrainMeters,
            planet.radiusMeters +
                kMinimumObserverAboveReferenceMeters);

    return {
        .meters =
            safeDirection *
            radius
    };
}

[[nodiscard]] StudioTerrainNavigationUpdate BuildUpdate(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain,
    const terrain::TerrainSource& source,
    const world::WorldPosition& observer,
    const camera::FreeCameraUpdate& localCamera,
    const bool moved)
{
    const f64 altitude =
        ObserverAltitudeAboveTerrain(
            terrain.planet,
            source,
            observer);

    return {
        .observer =
            observer,
        .surfaceFrame =
            state.surfaceFrame,
        .localCamera =
            localCamera,
        .altitudeAboveTerrainMeters =
            altitude,
        .moved =
            moved
    };
}
} // namespace

bool StudioTerrainNavigationConfig::IsValid() const noexcept
{
    return
        std::isfinite(
            movementSpeedScale) &&
        movementSpeedScale > 0.0 &&
        std::isfinite(
            minimumGroundClearanceMeters) &&
        minimumGroundClearanceMeters > 0.0 &&
        std::isfinite(
            focusSurfaceHeightMeters) &&
        focusSurfaceHeightMeters >
            minimumGroundClearanceMeters &&
        std::isfinite(
            focusOrbitAltitudeRadiusFraction) &&
        focusOrbitAltitudeRadiusFraction > 0.0 &&
        std::isfinite(
            focusOrbitMinimumAltitudeMeters) &&
        focusOrbitMinimumAltitudeMeters > 0.0 &&
        std::isfinite(
            focusOrbitPitchRadians) &&
        std::isfinite(
            focusSurfacePitchRadians);
}

void SynchronizeTerrainNavigation(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain)
{
    if (!state.config.IsValid())
    {
        throw std::invalid_argument(
            "Studio terrain navigation configuration is invalid.");
    }

    if (terrain.planet.radiusMeters <= 0.0 ||
        !std::isfinite(
            terrain.planet.radiusMeters))
    {
        throw std::invalid_argument(
            "Studio terrain navigation requires a valid spherical planet.");
    }

    const math::Double3 direction =
        ObserverDirection(
            terrain.observer);

    const bool identityChanged =
        !state.initialized ||
        state.worldGeneration !=
            terrain.worldGeneration ||
        state.universeGeneration !=
            terrain.universeGeneration ||
        state.semanticBody !=
            terrain.semanticBody ||
        state.body !=
            terrain.body;

    if (identityChanged)
    {
        state.worldGeneration =
            terrain.worldGeneration;
        state.universeGeneration =
            terrain.universeGeneration;
        state.semanticBody =
            terrain.semanticBody;
        state.body =
            terrain.body;
        state.surfaceFrame =
            world::MakeSurfaceFrame(
                direction);
        state.freeCamera =
            camera::FreeCamera{};
        state.initialized =
            true;
        return;
    }

    state.surfaceFrame =
        world::TransportSurfaceFrameToDirection(
            state.surfaceFrame,
            direction);
}

StudioTerrainNavigationUpdate
CurrentTerrainNavigation(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain,
    const terrain::TerrainSource& source)
{
    SynchronizeTerrainNavigation(
        state,
        terrain);

    return BuildUpdate(
        state,
        terrain,
        source,
        terrain.observer,
        state.freeCamera.Current(),
        false);
}

StudioTerrainNavigationUpdate
AdvanceTerrainNavigation(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain,
    const terrain::TerrainSource& source,
    const StudioTerrainNavigationInput& input)
{
    SynchronizeTerrainNavigation(
        state,
        terrain);

    const f64 observerRadius =
        math::Length(
            terrain.observer.meters);

    const f64 altitudeAboveTerrain =
        ObserverAltitudeAboveTerrain(
            terrain.planet,
            source,
            terrain.observer);

    camera::FreeCameraUpdate localCamera =
        state.freeCamera.Update({
            .deltaSeconds =
                std::max(
                    input.deltaSeconds,
                    0.0),
            .mouseDeltaX =
                input.mouseDeltaX,
            .mouseDeltaY =
                input.mouseDeltaY,
            .moveRight =
                input.moveRight,
            .moveForward =
                input.moveForward,
            .moveUp =
                input.moveUp,
            .boost =
                input.boost,
            .altitudeMeters =
                std::max(
                    altitudeAboveTerrain,
                    state.config.
                        minimumGroundClearanceMeters)
        });

    const f64 speedScale =
        std::clamp(
            state.config.
                movementSpeedScale,
            0.01,
            1'000.0);

    const math::Double2 tangentMotion{
        localCamera.
                tangentMotionMeters.x *
            speedScale,
        localCamera.
                tangentMotionMeters.y *
            speedScale
    };

    const f64 verticalMotion =
        localCamera.
            verticalMotionMeters *
        speedScale;

    world::SurfaceFrame newFrame =
        world::SurfaceFrameAtOffset(
            terrain.planet,
            state.surfaceFrame,
            tangentMotion);

    const f64 projectedReferenceAltitude =
        std::max(
            observerRadius -
                terrain.planet.radiusMeters +
                verticalMotion,
            0.0);

    const f64 newElevation =
        SampleElevation(
            source,
            terrain.planet,
            newFrame.up,
            std::max(
                projectedReferenceAltitude *
                    0.02,
                1.0));

    const f64 minimumRadius =
        std::max(
            terrain.planet.radiusMeters +
                newElevation +
                state.config.
                    minimumGroundClearanceMeters,
            terrain.planet.radiusMeters +
                kMinimumObserverAboveReferenceMeters);

    const f64 newRadius =
        std::max(
            observerRadius +
                verticalMotion,
            minimumRadius);

    const world::WorldPosition observer{
        .meters =
            newFrame.up *
            newRadius
    };

    state.surfaceFrame =
        newFrame;

    const bool positionMoved =
        math::LengthSquared(
            observer.meters -
            terrain.observer.meters) >
        1.0e-18;

    return BuildUpdate(
        state,
        terrain,
        source,
        observer,
        localCamera,
        positionMoved);
}

StudioTerrainNavigationUpdate
FocusTerrainBody(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain,
    const terrain::TerrainSource& source)
{
    SynchronizeTerrainNavigation(
        state,
        terrain);

    const math::Double3 direction =
        ObserverDirection(
            terrain.observer);

    const f64 altitude =
        std::max(
            terrain.planet.radiusMeters *
                state.config.
                    focusOrbitAltitudeRadiusFraction,
            state.config.
                focusOrbitMinimumAltitudeMeters);

    const world::WorldPosition observer =
        ObserverAtDirection(
            terrain.planet,
            source,
            direction,
            altitude);

    state.surfaceFrame =
        world::MakeSurfaceFrame(
            direction);

    camera::FreeCameraConfig cameraConfig{};
    cameraConfig.initialPitchRadians =
        state.config.
            focusOrbitPitchRadians;
    state.freeCamera =
        camera::FreeCamera(
            cameraConfig);

    return BuildUpdate(
        state,
        terrain,
        source,
        observer,
        state.freeCamera.Current(),
        true);
}

StudioTerrainNavigationUpdate
FocusTerrainSurface(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain,
    const terrain::TerrainSource& source,
    const math::Double3& unitDirection)
{
    SynchronizeTerrainNavigation(
        state,
        terrain);

    const math::Double3 direction =
        math::Normalize(
            unitDirection);

    if (math::LengthSquared(direction) <=
        1.0e-20)
    {
        throw std::invalid_argument(
            "Studio terrain surface focus requires a valid unit direction.");
    }

    const f64 height =
        std::max(
            state.config.
                focusSurfaceHeightMeters,
            state.config.
                minimumGroundClearanceMeters);

    const world::WorldPosition observer =
        ObserverAtDirection(
            terrain.planet,
            source,
            direction,
            height);

    state.surfaceFrame =
        world::MakeSurfaceFrame(
            direction);

    camera::FreeCameraConfig cameraConfig{};
    cameraConfig.initialPitchRadians =
        state.config.
            focusSurfacePitchRadians;
    state.freeCamera =
        camera::FreeCamera(
            cameraConfig);

    return BuildUpdate(
        state,
        terrain,
        source,
        observer,
        state.freeCamera.Current(),
        true);
}

StudioTerrainNavigationUpdate
ResetTerrainNavigationOrientation(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain,
    const terrain::TerrainSource& source)
{
    SynchronizeTerrainNavigation(
        state,
        terrain);

    state.freeCamera =
        camera::FreeCamera{};

    return BuildUpdate(
        state,
        terrain,
        source,
        terrain.observer,
        state.freeCamera.Current(),
        false);
}

StudioTerrainClipPlanes
SurfaceSafeTerrainClipPlanes(
    const world::PlanetDefinition& planet,
    const f64 altitudeAboveTerrainMeters) noexcept
{
    const f64 radius =
        std::max(
            planet.radiusMeters,
            1.0);

    const f64 altitude =
        std::max(
            altitudeAboveTerrainMeters,
            0.0);

    const f64 nearPlane =
        std::clamp(
            altitude * 0.002,
            0.02,
            5'000.0);

    const f64 observerRadius =
        radius +
        altitude;

    const f64 horizon =
        std::sqrt(
            std::max(
                observerRadius *
                    observerRadius -
                radius * radius,
                0.0));

    const f64 farPlane =
        std::max({
            20'000.0,
            nearPlane * 1'000.0,
            horizon * 1.75 +
                altitude * 2.0
        });

    return {
        .nearPlaneMeters =
            static_cast<f32>(
                nearPlane),
        .farPlaneMeters =
            static_cast<f32>(
                farPlane)
    };
}
} // namespace orbit::studio_ui
