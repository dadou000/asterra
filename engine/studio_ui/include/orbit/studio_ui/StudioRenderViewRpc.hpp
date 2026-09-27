#pragma once

#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>

namespace orbit::studio_ui
{
// Registers RPC/MCP parity for Studio RenderView presentation state that has
// no other authoritative source (ORBIT_MCP.md, AGENTS.md "MCP parity"). The
// dispatcher and render-view set must outlive the registration.
void RegisterStudioRenderViewRpc(
    rpc::Dispatcher& dispatcher,
    StudioRenderViewSet& views);
} // namespace orbit::studio_ui
