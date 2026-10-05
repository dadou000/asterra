#pragma once

#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>

namespace orbit::studio_ui
{
// RPC/MCP parity for the Display Diagnostics eye-adaptation controls
// (display.eye_get / display.eye_set / display.eye_reset). The dispatcher and
// renderer must outlive the registration.
void RegisterDisplayEyeRpc(
    rpc::Dispatcher& dispatcher,
    StudioViewportRenderer& renderer);
} // namespace orbit::studio_ui
