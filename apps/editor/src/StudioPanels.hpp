#pragma once

// Shared plumbing for the Studio panels that main() used to register inline. A panel class owns the state its
// draw callbacks used to capture and reaches the application through the references below, under the same names
// the callbacks always used (ui, worldSession, content, ...) and the same world-session accessors (objects(),
// selection(), ...).

#include <orbit/commands/CommandRegistry.hpp>
#include <orbit/commands/CommandService.hpp>
#include <orbit/content/ContentService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/documents/WorldDatabase.hpp>
#include <orbit/editor_model/CommandSurfaces.hpp>
#include <orbit/editor_model/ExplorerModel.hpp>
#include <orbit/editor_model/InspectorModel.hpp>
#include <orbit/editor_model/OutputLog.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/editor_ui/BodyPreviewRenderer.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/platform/Window.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/runtime/RuntimeSession.hpp>
#include <orbit/schema/SchemaRegistry.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>
#include <orbit/studio_ui/StudioTextDiagnosticsHud.hpp>
#include <orbit/studio_ui/StudioViewportPanels.hpp>
#include <orbit/studio_ui/ViewportCaptureService.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/world_model/UniverseComposition.hpp>

#include <orbit/platform_services/PlatformConfig.hpp>
#include <orbit/plugins/PluginManager.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit::editor_app
{
inline constexpr editor_ui::PanelId
    kViewportPanel{
        .high =
            0x4f52424954535455ULL,
        .low =
            0x44494f5657455750ULL
    };

inline constexpr editor_ui::PanelId
    kExplorerPanel{
        .high =
            0x4f52424954535455ULL,
        .low =
            0x44494f4558504c52ULL
    };

inline constexpr editor_ui::PanelId
    kPropertiesPanel{
        .high =
            0x4f52424954535455ULL,
        .low =
            0x44494f50524f5053ULL
    };

inline constexpr editor_ui::PanelId
    kOutputPanel{
        .high =
            0x4f52424954535455ULL,
        .low =
            0x44494f4f55545054ULL
    };

inline constexpr editor_ui::PanelId
    kBuildPanel{
        .high =
            0x4f52424954535455ULL,
        .low =
            0x44494f4255494c44ULL
    };

inline constexpr editor_ui::PanelId
    kPlatformServicesPanel{
        .high =
            0x4f52424954535455ULL,
        .low =
            0x44494f504c415446ULL
    };

inline constexpr editor_ui::PanelId
    kContentPanel{
        .high =
            0x4f52424954535455ULL,
        .low =
            0x44494f434f4e544eULL
    };

inline constexpr editor_ui::PanelId
    kPluginsPanel{
        .high =
            0x4f52424954535455ULL,
        .low =
            0x44494f504c554749ULL
    };

struct StudioPanelEnvironment
{
    editor_ui::EditorUi& ui;
    editor_session::EditorWorldSession& worldSession;
    studio_session::StudioSession& studioSession;
    documents::ProjectDocument& project;
    content::ContentService& content;
    runtime::RuntimeSession& runtime;
    platform::Window& window;
    editor_model::OutputLog& outputLog;
    render_view::RenderView& materialView;
    std::optional<std::pair<u32, u32>>& pendingMaterialViewResize;
    std::function<std::vector<editor_ui::ActionPresentation>(
        std::string_view,
        editor_model::CommandSurfaceKind)>
        presentActions;
    studio_ui::StudioRenderViewSet& studioViews;
    studio_ui::StudioTextDiagnosticsHud& primaryTextHud;
    studio_ui::StudioViewportPanels& studioViewportPanels;
    studio_ui::ViewportCaptureService& viewportCapture;
    std::optional<std::pair<u32, u32>>& pendingPrimaryViewResize;
    world_model::UniverseComposition& universe;
    scene::ObjectId& bodyObject;
    universe::BodyId& bodyId;
};

class StudioPanelBase
{
public:
    explicit StudioPanelBase(StudioPanelEnvironment& environment) noexcept
        : ui(environment.ui),
          worldSession(environment.worldSession),
          studioSession(environment.studioSession),
          project(environment.project),
          content(environment.content),
          runtime(environment.runtime),
          window(environment.window),
          outputLog(environment.outputLog),
          materialView(environment.materialView),
          pendingMaterialViewResize(environment.pendingMaterialViewResize),
          presentActions(environment.presentActions),
          studioViews(environment.studioViews),
          primaryTextHud(environment.primaryTextHud),
          studioViewportPanels(environment.studioViewportPanels),
          viewportCapture(environment.viewportCapture),
          pendingPrimaryViewResize(environment.pendingPrimaryViewResize),
          universe(environment.universe),
          bodyObject(environment.bodyObject),
          bodyId(environment.bodyId)
    {
    }

protected:
    editor_ui::EditorUi& ui;
    editor_session::EditorWorldSession& worldSession;
    studio_session::StudioSession& studioSession;
    documents::ProjectDocument& project;
    content::ContentService& content;
    runtime::RuntimeSession& runtime;
    platform::Window& window;
    editor_model::OutputLog& outputLog;
    render_view::RenderView& materialView;
    std::optional<std::pair<u32, u32>>& pendingMaterialViewResize;
    std::function<std::vector<editor_ui::ActionPresentation>(
        std::string_view,
        editor_model::CommandSurfaceKind)>
        presentActions;
    studio_ui::StudioRenderViewSet& studioViews;
    studio_ui::StudioTextDiagnosticsHud& primaryTextHud;
    studio_ui::StudioViewportPanels& studioViewportPanels;
    studio_ui::ViewportCaptureService& viewportCapture;
    std::optional<std::pair<u32, u32>>& pendingPrimaryViewResize;
    world_model::UniverseComposition& universe;
    scene::ObjectId& bodyObject;
    universe::BodyId& bodyId;

    // The world-session services, resolved on use so no reference survives a world swap.
    [[nodiscard]] documents::WorldDatabase& world() const
    {
        return worldSession.World();
    }
    [[nodiscard]] schema::SchemaRegistry& schemas() const
    {
        return worldSession.Schemas();
    }
    [[nodiscard]] scene::ObjectStore& objects() const
    {
        return worldSession.Objects();
    }
    [[nodiscard]] selection::SelectionService& selection() const
    {
        return worldSession.Selection();
    }
    [[nodiscard]] commands::CommandService& commandService() const
    {
        return worldSession.Commands();
    }
    [[nodiscard]] editor_model::ExplorerModel& explorer() const
    {
        return worldSession.Explorer();
    }
    [[nodiscard]] editor_model::InspectorModel& inspector() const
    {
        return worldSession.Inspector();
    }
    [[nodiscard]] plugins::PluginManager& plugins() const
    {
        return worldSession.Plugins();
    }
    [[nodiscard]] commands::CommandRegistry& authoringCommands() const
    {
        return worldSession.CommandRegistry();
    }
    [[nodiscard]] universe::BodyRegistry& bodies() const
    {
        return worldSession.Universe().Bodies();
    }
    [[nodiscard]] frames::FrameGraph& frames() const
    {
        return worldSession.Universe().Frames();
    }
};

class StudioViewportPanel : public StudioPanelBase
{
public:
    using StudioPanelBase::StudioPanelBase;
    void Register();

    // Viewport and path-placement interaction state. The frame loop reads and resets the parts it
    // drives (frame mouse delta, right-button gesture, path placement mode).
    bool pathPlacementMode = false;
    bool pathDebugVisualization = true;
    std::optional<orbit::paths::NetworkId>
        activePathNetwork;
    std::optional<orbit::scene::ObjectId>
        lastPlacedPathNode;
    orbit::f64 viewportNavigationSpeedScale = 1.0;
    orbit::f64 viewportFrameDeltaSeconds = 0.0;
    orbit::platform::MouseDelta viewportFrameMouseDelta{};
    bool viewportRightGestureActive = false;
    bool viewportRightGestureDragged = false;
    orbit::i32 viewportRightGestureDistance = 0;
    bool viewportHomeWasDown = false;
    bool viewportEndWasDown = false;
};

class StudioExplorerPanel : public StudioPanelBase
{
public:
    using StudioPanelBase::StudioPanelBase;
    void Register();

    // State the draw callbacks keep between frames.
    std::string explorerSearch;
    std::string renameBuffer;
    orbit::u64 renameSelectionRevision =
        ~orbit::u64{0};
};

class StudioPropertiesPanel : public StudioPanelBase
{
public:
    using StudioPanelBase::StudioPanelBase;
    void Register();

    // State the draw callbacks keep between frames.
    bool showAdvancedProperties = false;
};

class StudioPluginsPanel : public StudioPanelBase
{
public:
    using StudioPanelBase::StudioPanelBase;
    void Register();

    // State the draw callbacks keep between frames.
    std::vector<
        orbit::plugins::PluginValidationIssue>
        pluginValidationIssues;
};

class StudioMaterialServicePanel : public StudioPanelBase
{
public:
    using StudioPanelBase::StudioPanelBase;
    void Register();

    // State the draw callbacks keep between frames.
    std::string contentSearch;
    std::optional<orbit::content::AssetId>
        materialPreviewAsset;
    orbit::editor_ui::PreviewMaterial
        materialPreviewMaterial{};
    std::optional<orbit::content::AssetId>
        materialEmissionEditAsset;
    orbit::content::MaterialEmission
        materialEmissionEdit{};
    std::string materialEmissiveTextureEdit;
    std::string materialEmissionStatus;
};

class StudioPlatformServicesPanel : public StudioPanelBase
{
public:
    explicit StudioPlatformServicesPanel(StudioPanelEnvironment& environment);
    void Register();

    // State the draw callbacks keep between frames.
    const std::filesystem::path
        platformConfigurationPath =
            project.RootDirectory() /
            "Config" /
            "PlatformServices.toml";
    orbit::platform_services::
        PlatformConfiguration
            platformConfiguration;
    std::vector<
        orbit::platform_services::
            PlatformConfigIssue>
        platformConfigurationIssues;
    std::string platformStatus{
        "Not configured"};
    orbit::i64 steamAppIdEditor = 0;
    std::string newAchievementId;
    std::string newAchievementApiName;
    std::string newStatId;
    std::string newStatApiName;
    bool newStatFloat = false;
    std::string newTimelineEventId;
    std::string newTimelineTitle;
    std::string newTimelineDescription;
    std::string newTimelineIcon{
        "steam_marker"};
};

class StudioOutputPanel : public StudioPanelBase
{
public:
    using StudioPanelBase::StudioPanelBase;
    void Register();

    // State the draw callbacks keep between frames.
};
} // namespace orbit::editor_app
