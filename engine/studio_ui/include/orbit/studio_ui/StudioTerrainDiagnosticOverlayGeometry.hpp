#pragma once

#include <orbit/editor_ui/PathPreviewRenderer.hpp>
#include <orbit/studio_session/StudioTerrainPhysicalPageService.hpp>
#include <orbit/studio_session/StudioTerrainRuntimeBridge.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/terrain/TerrainSource.hpp>

#include <memory>
#include <span>
#include <vector>

namespace orbit::studio_ui
{
struct StudioTerrainDiagnosticPage
{
    studio_session::StudioTerrainPageRebuildStatus status{};
    std::shared_ptr<
        const studio_session::StudioTerrainPhysicalPageSnapshot>
        snapshot;
};

// M13 disposable diagnostic geometry. All inputs are copied/read-only runtime
// snapshots. Producing or toggling these lines cannot invalidate or regenerate
// terrain by construction.
[[nodiscard]] std::vector<editor_ui::PreviewLine>
BuildTerrainDiagnosticOverlayLines(
    const StudioTerrainDiagnosticOverlayOptions& options,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::TerrainSource& source,
    std::span<const StudioTerrainDiagnosticPage> pages,
    const render_view::CameraState& camera);
} // namespace orbit::studio_ui
