#pragma once

#include <orbit/rpc/JsonRpc.hpp>
namespace orbit::editor_ui { class EditorUi; }
namespace orbit::studio_session { class StudioSession; }
namespace orbit::studio_ui
{
class ProjectAuthoringUi;
class StudioRenderViewSet;
class StudioViewportPanels;
}

namespace orbit::editor_app
{
void RegisterStudioProjectRpc(rpc::Dispatcher& dispatcher, studio_ui::ProjectAuthoringUi& projectBrowserUi);
void RegisterStudioWorkspaceRpc(rpc::Dispatcher& dispatcher, editor_ui::EditorUi& ui, studio_ui::StudioRenderViewSet& studioViews, studio_session::StudioSession& studioSession, studio_ui::StudioViewportPanels& studioViewportPanels);

} // namespace orbit::editor_app
