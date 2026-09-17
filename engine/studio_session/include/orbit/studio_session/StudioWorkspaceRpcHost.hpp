#pragma once

#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_session/StudioWorkspace.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_session
{
// Stable JSON-RPC boundary above project lifetime. Workspace methods remain
// callable with no project open; all other requests are delegated to the
// current StudioSession. Project switches are standalone requests so no batch
// can continue against a destroyed project/session graph.
class StudioWorkspaceRpcHost
{
public:
    explicit StudioWorkspaceRpcHost(
        StudioWorkspace& workspace);

    [[nodiscard]] std::optional<std::string>
    Dispatch(std::string_view payload);

    [[nodiscard]] rpc::Dispatcher& Dispatcher() noexcept;
    [[nodiscard]] const rpc::Dispatcher& Dispatcher() const noexcept;

private:
    void RegisterMethods();

    [[nodiscard]] bool IsWorkspaceMethod(
        std::string_view method) const noexcept;

    [[nodiscard]] bool BatchContainsWorkspaceMethod(
        const rpc::Value& document) const noexcept;

    [[nodiscard]] std::string NoProjectError(
        const rpc::Value& request) const;

    StudioWorkspace& workspace_;
    rpc::Dispatcher dispatcher_;
    std::vector<std::string> methods_;
};
} // namespace orbit::studio_session
