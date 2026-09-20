#pragma once

#include <orbit/editor_ui/PathPreviewRenderer.hpp>
#include <orbit/studio_session/StudioTerrainRuntimeBridge.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/terrain/TerrainSource.hpp>

#include <vector>

namespace orbit::studio_ui
{
// Disposable render geometry only. Authoritative directions/radii remain in
// the semantic M09 constraint or transient StudioRenderViewSet tool state.
[[nodiscard]] std::vector<editor_ui::PreviewLine>
BuildTerrainAuthoringOverlayLines(
    const StudioTerrainAuthoringOverlay& overlay,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    const render_view::CameraState& camera);
} // namespace orbit::studio_ui
