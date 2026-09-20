#pragma once

#include <orbit/render_view/RenderView.hpp>
#include <orbit/studio_session/StudioTerrainRuntimeBridge.hpp>
#include <orbit/studio_session/ViewportTargetRegistry.hpp>
#include <orbit/terrain/TerrainContracts.hpp>

#include <optional>

namespace orbit::studio_ui
{
// Converts one generation-stamped logical Studio viewport target into the
// camera consumed by RenderView. Missing targets are valid (blank worlds),
// while stale universe targets are rejected rather than rendered against a
// replacement FrameGraph/BodyRegistry.
[[nodiscard]] std::optional<render_view::CameraState>
ComposeViewportCamera(
    const studio_session::ViewportTargetState& view,
    u64 universeGeneration);

// Perspective terrain camera aligned with the production renderer's
// observer-local basis (east, up, north). The returned CameraState is expressed
// in the body's frame so existing editor ray/picking code shares the same
// physical observer as the rendered terrain.
[[nodiscard]] render_view::CameraState
ComposeTerrainViewportCamera(
    const studio_session::ViewportTargetState& view,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& terrain);

struct StudioPhysicalPageSelection
{
    terrain::PhysicalTerrainPageAddress address{};
    math::Double3 surfaceDirection{};
};

[[nodiscard]] std::optional<StudioPhysicalPageSelection>
PhysicalPageAtViewportPoint(
    const studio_session::ViewportTargetState& view,
    const render_view::CameraState& camera,
    u32 width,
    u32 height,
    f32 u,
    f32 v,
    u8 physicalTileLevel) noexcept;

void ApplyViewportCamera(
    render_view::RenderView& view,
    const render_view::CameraState& camera) noexcept;
} // namespace orbit::studio_ui
