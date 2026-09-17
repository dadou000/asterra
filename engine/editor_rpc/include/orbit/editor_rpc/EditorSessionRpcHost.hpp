#pragma once

#include <orbit/editor_rpc/EditorRpcService.hpp>
#include <orbit/editor_session/EditorWorldSession.hpp>
#include <orbit/rpc/JsonRpc.hpp>

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
};
} // namespace orbit::editor_rpc
