#pragma once

#include <orbit/render_view/RenderView.hpp>
#include <orbit/studio_session/ViewportTargetRegistry.hpp>

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

void ApplyViewportCamera(
    render_view::RenderView& view,
    const render_view::CameraState& camera) noexcept;
} // namespace orbit::studio_ui
