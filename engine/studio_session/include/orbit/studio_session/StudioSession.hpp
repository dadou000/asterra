#pragma once

#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_rpc/EditorSessionRpcHost.hpp>
#include <orbit/editor_session/ActiveBodyModel.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/editor_session/WorldDocumentsModel.hpp>
#include <orbit/studio_session/UniverseBoundPathCache.hpp>
#include <orbit/studio_session/UniverseBoundRoutePlanner.hpp>
#include <orbit/studio_session/ViewportTargetRegistry.hpp>
#include <orbit/studio_session/WorldBoundPathNetwork.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_session
{
struct StudioTickResult
{
    bool activeBodyChanged{false};
    bool viewportTargetsChanged{false};
    bool pathNetworkRebound{false};
    bool pathRoutingRebound{false};
    bool pathProductsInvalidated{false};
    u32 pluginsReloaded{0};
    u64 worldGeneration{0};
    u64 universeGeneration{0};
};

// Application-facing composition root for Orbit Studio's authoritative
// authoring session. World lifecycle always passes through the session-aware
// RPC boundary so its world-bound handlers are rebound together with the
// EditorWorldSession. UI, MCP and plugins therefore observe one service graph.
class StudioSession
{
public:
    explicit StudioSession(
        documents::ProjectDocument& project);

    [[nodiscard]] editor_session::EditorWorldSession&
    World() noexcept;
    [[nodiscard]] const editor_session::EditorWorldSession&
    World() const noexcept;

    [[nodiscard]] editor_session::ActiveBodyModel&
    ActiveBody() noexcept;
    [[nodiscard]] const editor_session::ActiveBodyModel&
    ActiveBody() const noexcept;

    [[nodiscard]] ViewportTargetRegistry&
    Viewports() noexcept;
    [[nodiscard]] const ViewportTargetRegistry&
    Viewports() const noexcept;

    [[nodiscard]] WorldBoundPathNetwork&
    PathNetwork() noexcept;
    [[nodiscard]] const WorldBoundPathNetwork&
    PathNetwork() const noexcept;

    [[nodiscard]] UniverseBoundRoutePlanner&
    PathRouting() noexcept;
    [[nodiscard]] const UniverseBoundRoutePlanner&
    PathRouting() const noexcept;

    [[nodiscard]] UniverseBoundPathCache&
    PathProducts() noexcept;
    [[nodiscard]] const UniverseBoundPathCache&
    PathProducts() const noexcept;

    [[nodiscard]] std::vector<editor_session::WorldDocumentItem>
    Worlds() const;
    [[nodiscard]] std::optional<editor_session::WorldDocumentItem>
    ActiveWorld() const;

    [[nodiscard]] documents::WorldDescriptor CreateWorld(
        const std::filesystem::path& relativePath,
        std::string_view displayName);
    [[nodiscard]] documents::WorldDescriptor RenameWorld(
        const std::filesystem::path& relativePath,
        std::string_view displayName);
    [[nodiscard]] documents::WorldDescriptor SetStartupWorld(
        const std::filesystem::path& relativePath);

    void OpenWorld(
        const std::filesystem::path& relativePath);
    void CloseWorld();

    [[nodiscard]] std::optional<std::string>
    DispatchRpc(std::string_view payload);

    [[nodiscard]] editor_rpc::EditorSessionRpcHost&
    Rpc() noexcept;
    [[nodiscard]] const editor_rpc::EditorSessionRpcHost&
    Rpc() const noexcept;

    [[nodiscard]] StudioTickResult Tick();

private:
    void DispatchWorldLifecycle(
        std::string_view method,
        std::optional<std::filesystem::path> path);

    editor_session::EditorWorldSession world_;
    editor_session::WorldDocumentsModel documents_;
    editor_session::ActiveBodyModel activeBody_;
    ViewportTargetRegistry viewports_;
    WorldBoundPathNetwork pathNetwork_;
    UniverseBoundRoutePlanner pathRouting_;
    UniverseBoundPathCache pathProducts_;
    editor_rpc::EditorSessionRpcHost rpc_;
};
} // namespace orbit::studio_session
