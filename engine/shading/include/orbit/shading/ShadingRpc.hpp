#pragma once

#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/shading/ShadingWorkspace.hpp>

#include <filesystem>
#include <functional>

namespace orbit::shading
{
struct ShadingRpcHooks
{
    // Captures the Shading preview to a BMP and returns its description
    // ({"path", "width", "height", ...}). Throws on failure. Optional: without
    // it shading.screenshot reports that no preview target is attached.
    std::function<rpc::Value(const std::filesystem::path& path)> screenshot;
};

// Registers the `shading.*` methods (documented in docs/ORBIT_SHADING.md and
// docs/ORBIT_MCP.md). `workspace` must outlive the dispatcher's use of them.
//
// Error codes: 1050 invalid request (bad path/name/parameter, refused
// operation), 1051 unexpected failure.
void RegisterShadingRpc(
    rpc::Dispatcher& dispatcher,
    ShadingWorkspace& workspace,
    ShadingRpcHooks hooks = {});

// Names registered, so a host can unregister them on teardown.
[[nodiscard]] std::vector<std::string> ShadingRpcMethodNames();
} // namespace orbit::shading
