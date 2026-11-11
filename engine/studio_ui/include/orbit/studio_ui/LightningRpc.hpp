#pragma once

#include <orbit/lighting/TransientFlashRegistry.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_ui/StormLightningController.hpp>

#include <functional>
#include <vector>

namespace orbit::studio_ui
{
// What the lightning RPC needs from the viewport that owns the flashes. Kept as callables so the RPC is testable
// without a GPU device.
struct LightningHost
{
    std::function<u64(const lighting::FlashRequest&)> fire;
    std::function<std::vector<lighting::FlashStatus>()> status;
    std::function<void()> clear;
    // Weather Lab driven lightning (StormLightningController). Empty string from stormStart = started.
    std::function<std::string(const StormLightningRequest&)> stormStart;
    std::function<void()> stormStop;
    std::function<StormLightningStatus()> stormStatus;
};

// lightning.fire / lightning.status / lightning.clear: fire a lightning flash that lights the scene and the clouds, list
// the live flashes, remove them (docs/ORBIT_MCP.md). `host` must outlive the dispatcher.
void RegisterLightningRpc(rpc::Dispatcher& dispatcher, LightningHost host);
} // namespace orbit::studio_ui
