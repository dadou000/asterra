#pragma once

#include <orbit/editor_model/ViewportManipulator.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/studio_ui/ViewportAuthoringState.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace orbit::studio_ui
{
// The on-screen Move / Rotate / Scale handles of the viewport. This class
// only projects handles, hit-tests them and forwards pointer rays to
// editor_model::ViewportManipulator, which owns all transform math and writes
// through CommandService (one undo step per drag). The same operation without
// a pointer is the object.transform RPC.
class StudioViewportManipulatorUi
{
public:
    StudioViewportManipulatorUi();
    ~StudioViewportManipulatorUi();

    StudioViewportManipulatorUi(
        const StudioViewportManipulatorUi&) = delete;
    StudioViewportManipulatorUi& operator=(
        const StudioViewportManipulatorUi&) = delete;

    // Call right after the viewport Image is submitted (it overlays and reads
    // the pointer relative to that last item). Draws the handles of the
    // single selected object for gizmo.tool and runs the drag. Returns true
    // while the gizmo owns the left button (hovering a handle or dragging),
    // so the caller must not treat that press as a selection click.
    [[nodiscard]] bool Handle(
        editor_ui::PanelContext& context,
        StudioRenderViewSet& views,
        studio_session::StudioSession& session,
        std::string_view viewId,
        const GizmoSettings& gizmo);

    // Abandons a drag in progress and restores the object.
    void Cancel() noexcept;

    [[nodiscard]] bool Dragging() const noexcept;

private:
    struct DragState;

    std::unique_ptr<DragState> drag_;
};
} // namespace orbit::studio_ui
