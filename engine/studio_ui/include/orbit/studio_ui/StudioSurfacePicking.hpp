#pragma once

#include <orbit/render_view/RenderView.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/studio_session/StudioTerrainRuntimeBridge.hpp>
#include <orbit/studio_session/ViewportTargetRegistry.hpp>
#include <orbit/terrain/TerrainPosition.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <optional>
#include <string>

namespace orbit::studio_ui
{
// Exact provenance of the normalized viewport ray used for one terrain pick.
// Pixel dimensions are retained only to reproduce the ray; canonical surface
// identity never depends on render-target size or cube-face representation.
struct StudioViewportRayProvenance
{
    std::string viewportId;

    u32 width{0U};
    u32 height{0U};
    f32 u{0.0F};
    f32 v{0.0F};

    render_view::ViewRay ray{};

    u64 universeGeneration{0U};
    u64 terrainRuntimeGeneration{0U};
    u64 terrainSourceRevision{0U};
};

struct StudioSurfacePick
{
    scene::ObjectId semanticBody{};
    universe::BodyId body{};

    terrain::PlanetSurfacePosition surface{};

    // TerrainSource retains physical ground separately from standing water.
    // Production rendering displays elevation + water depth, so authoring
    // tools can deliberately choose physical or visible placement.
    f64 physicalElevationMeters{0.0};
    f64 renderedElevationMeters{0.0};
    f64 hitDistanceMeters{0.0};

    std::optional<terrain::PhysicalTerrainPageAddress>
        physicalPage;
    std::optional<u8> physicalLod;

    StudioViewportRayProvenance provenance{};
};

// Shared M08 picking seam for selection, debug-page selection and future
// constraint/biome/path tools. It samples the same TerrainSource consumed by
// production terrain instead of maintaining an editor-only height model.
[[nodiscard]] std::optional<StudioSurfacePick>
PickStudioTerrainSurface(
    const studio_session::ViewportTargetState& viewport,
    const render_view::CameraState& camera,
    u32 width,
    u32 height,
    f32 u,
    f32 v,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrainRuntime,
    const terrain::TerrainSource& source,
    std::optional<u8> physicalTileLevel = std::nullopt) noexcept;
} // namespace orbit::studio_ui
