#pragma once

#include <orbit/rpc/JsonRpc.hpp>

#include <functional>
#include <string>

namespace orbit::studio_ui
{
// The split view of the cloud march: left of `position` the shipped defines, right of it `defines`.
struct CloudSplitState
{
    bool enabled{false};
    std::string defines;
    f32 position{0.5F};
};

// What the cloud shader RPC needs from the viewport renderer. Callables, so the RPC is testable without a GPU device.
struct CloudShaderHost
{
    // The cloud shader defines in use ("NAME=VALUE;NAME=VALUE").
    std::function<std::string()> get;
    // Recompiles the cloud shaders with the given defines; returns the compile error or an empty string. On error the
    // previous shaders stay in use.
    std::function<std::string(const std::string&)> set;
    // The split view in use, and a setter (enabled, defines of the right half, divider 0..1); returns the compile error or an empty
    // string. Optional: without them clouds.split_* are not registered.
    std::function<CloudSplitState()> getSplit;
    std::function<std::string(bool, const std::string&, f32)> setSplit;
};

// clouds.get_defines / clouds.set_defines: change the cloud march tunables (CLOUD_MAX_STEPS, CLOUD_FAR_EARLYOUT, ...) in the
// running Studio, so an A/B comparison needs no relaunch (tools/orbit_ab.py, docs/ORBIT_MCP.md). `host` must outlive the
// dispatcher.
void RegisterCloudShaderRpc(rpc::Dispatcher& dispatcher, CloudShaderHost host);
} // namespace orbit::studio_ui
