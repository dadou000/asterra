#pragma once

#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_ui/StudioRenderViewSet.hpp>

namespace orbit::studio_ui
{
// Registers RPC/MCP parity for the flat planet map: opening it in a viewport,
// choosing its layer, reading its progress and marker, and travelling to a
// point on it (ORBIT_MCP.md, AGENTS.md "MCP parity"). The dispatcher and the
// render-view set must outlive the registration.
void RegisterStudioFlatMapRpc(
    rpc::Dispatcher& dispatcher,
    StudioRenderViewSet& views);
} // namespace orbit::studio_ui
