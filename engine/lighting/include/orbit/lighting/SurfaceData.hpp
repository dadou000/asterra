#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

namespace orbit::lighting
{
// Semantic category used by lighting/debugging. It is intentionally broader
// than a renderer or object type: representation changes must not change the
// physical meaning of the surface.
enum class SurfaceClass : u8
{
    Unknown,
    Terrain,
    Water,
    RigidGeometry,
    Vegetation,
    Character,
    CelestialSurface,
    VolumetricBoundary,
    Procedural
};

// Provenance of the representation that produced this visible surface.
// Lighting may use it for diagnostics/backend selection, never as a reason to
// switch to a different lighting model.
enum class SurfaceRepresentation : u8
{
    Unknown,
    ProductionSurface,
    MacroGlobe,
    SmoothGlobe,
    AnalyticImpostor,
    CachedImpostor,
    LocalMesh,
    ProceduralProxy
};

struct SurfaceData
{
    // GPU-facing position is camera-relative to retain precision. Stable
    // body/frame identity belongs to LightingView/cache addressing (M04).
    math::Float3 positionCameraRelativeMeters{};

    math::Float3 geometricNormal{0.0F, 1.0F, 0.0F};
    math::Float3 shadingNormal{0.0F, 1.0F, 0.0F};

    math::Float3 baseColorLinear{0.18F, 0.18F, 0.18F};
    f32 roughness{0.8F};
    f32 metallic{0.0F};

    // Scene-linear emitted radiance/energy representation. This is allowed to
    // exceed 1.0 and must never be display-clamped or tone-mapped here.
    math::Float3 emissionRadianceSceneLinear{};

    u32 materialId{0U};
    u32 instanceId{0U};
    SurfaceClass surfaceClass{SurfaceClass::Unknown};
    SurfaceRepresentation representation{
        SurfaceRepresentation::Unknown};
};

[[nodiscard]] bool IsFinite(
    const SurfaceData& surface) noexcept;

// Produces the canonical lighting-facing representation:
// - normals normalized with safe fallbacks;
// - base color / roughness / metallic restricted to physical material ranges;
// - negative/non-finite emission removed;
// - positive HDR emission is deliberately NOT upper-clamped.
[[nodiscard]] SurfaceData Canonicalize(
    const SurfaceData& surface) noexcept;
} // namespace orbit::lighting
