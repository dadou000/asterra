#pragma once

#include <orbit/editor_rpc/EditorRpcService.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/rpc/JsonRpc.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::editor_rpc
{
// Session-aware JSON-RPC composition boundary. Project/session methods remain
// alive across world switches while the world-bound EditorRpcService is
// destroyed and reconstructed against the new EditorWorldSession state.
class EditorSessionRpcHost
{
public:
    explicit EditorSessionRpcHost(
        editor_session::EditorWorldSession& session);
    ~EditorSessionRpcHost();

    EditorSessionRpcHost(
        const EditorSessionRpcHost&) = delete;
    EditorSessionRpcHost& operator=(
        const EditorSessionRpcHost&) = delete;

    [[nodiscard]] std::optional<std::string>
    Dispatch(std::string_view payload);

    [[nodiscard]] rpc::Dispatcher& Dispatcher() noexcept;
    [[nodiscard]] const rpc::Dispatcher& Dispatcher() const noexcept;

    [[nodiscard]] EditorRpcService* Editor() noexcept;
    [[nodiscard]] const EditorRpcService* Editor() const noexcept;

    // Persists application-owned automation attachments across world-service
    // reconstruction. The callback is applied immediately to the current
    // editor and again to every EditorRpcService created after world.open.
    void SetEditorConfigurator(
        std::function<void(EditorRpcService&)> configurator);

    void AttachViewport(
        ViewportAutomation viewport);
    void AttachPathRouting(
        PathRoutingAutomation routing);
    void AttachPathGeometry(
        PathGeometryAutomation geometry);
    void AttachBuild(
        BuildAutomation build);

    void PublishEvent(
        std::string type,
        rpc::Value data = {});
    [[nodiscard]] std::vector<std::string>
    DrainNotifications();

private:
    void RegisterHostMethods();
    void RegisterClosedProjectMethods();
    void UnregisterClosedProjectMethods() noexcept;
    void RebindEditor();

    [[nodiscard]] bool BatchContainsWorldSwitch(
        std::string_view payload) const noexcept;

    editor_session::EditorWorldSession& session_;
    rpc::Dispatcher dispatcher_;
    std::unique_ptr<EditorRpcService> editor_;
    std::vector<std::string> hostMethods_;
    std::vector<std::string> closedProjectMethods_;
    bool pendingRebind_{false};
    std::function<void(EditorRpcService&)>
        editorConfigurator_;
    std::optional<ViewportAutomation>
        viewportAutomation_;
    std::optional<PathRoutingAutomation>
        pathRoutingAutomation_;
    std::optional<PathGeometryAutomation>
        pathGeometryAutomation_;
    std::optional<BuildAutomation>
        buildAutomation_;
};
} // namespace orbit::editor_rpc
