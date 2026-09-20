#pragma once

#include <orbit/camera/FreeCamera.hpp>
#include <orbit/studio_session/StudioTerrainRuntimeBridge.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/world/Planet.hpp>
#include <orbit/world/WorldPosition.hpp>

namespace orbit::studio_ui
{
// Presentation-only controls for one production terrain viewport. Nothing in
// this state participates in terrain authority, generation revisions or cache
// identity.
struct StudioTerrainNavigationConfig
{
    f64 movementSpeedScale{1.0};
    f64 minimumGroundClearanceMeters{2.0};
    f64 focusSurfaceHeightMeters{120.0};
    f64 focusOrbitAltitudeRadiusFraction{0.25};
    f64 focusOrbitMinimumAltitudeMeters{100'000.0};
    f64 focusOrbitPitchRadians{-1.2217304763960306};
    f64 focusSurfacePitchRadians{-0.5235987755982988};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct StudioTerrainNavigationInput
{
    f64 deltaSeconds{0.0};

    f64 mouseDeltaX{0.0};
    f64 mouseDeltaY{0.0};

    f64 moveRight{0.0};
    f64 moveForward{0.0};
    f64 moveUp{0.0};

    bool boost{false};
};

struct StudioTerrainNavigationState
{
    StudioTerrainNavigationConfig config{};
    camera::FreeCamera freeCamera{};
    world::SurfaceFrame surfaceFrame{};

    u64 worldGeneration{0U};
    u64 universeGeneration{0U};
    scene::ObjectId semanticBody{};
    universe::BodyId body{};

    bool initialized{false};
};

struct StudioTerrainNavigationUpdate
{
    world::WorldPosition observer{};
    world::SurfaceFrame surfaceFrame{};
    camera::FreeCameraUpdate localCamera{};
    f64 altitudeAboveTerrainMeters{0.0};
    bool moved{false};
};

struct StudioTerrainClipPlanes
{
    f32 nearPlaneMeters{0.05F};
    f32 farPlaneMeters{20'000.0F};
};

void SynchronizeTerrainNavigation(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain);

[[nodiscard]] StudioTerrainNavigationUpdate
CurrentTerrainNavigation(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain,
    const terrain::TerrainSource& source);

[[nodiscard]] StudioTerrainNavigationUpdate
AdvanceTerrainNavigation(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain,
    const terrain::TerrainSource& source,
    const StudioTerrainNavigationInput& input);

[[nodiscard]] StudioTerrainNavigationUpdate
FocusTerrainBody(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain,
    const terrain::TerrainSource& source);

[[nodiscard]] StudioTerrainNavigationUpdate
FocusTerrainSurface(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain,
    const terrain::TerrainSource& source,
    const math::Double3& unitDirection);

[[nodiscard]] StudioTerrainNavigationUpdate
ResetTerrainNavigationOrientation(
    StudioTerrainNavigationState& state,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain,
    const terrain::TerrainSource& source);

[[nodiscard]] StudioTerrainClipPlanes
SurfaceSafeTerrainClipPlanes(
    const world::PlanetDefinition& planet,
    f64 altitudeAboveTerrainMeters) noexcept;
} // namespace orbit::studio_ui
