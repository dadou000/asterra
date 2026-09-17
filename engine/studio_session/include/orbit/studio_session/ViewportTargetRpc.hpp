#pragma once

#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_session/ViewportTargetRegistry.hpp>

namespace orbit::studio_session
{
// Registers persistent Studio viewport-target automation methods. The
// dispatcher and registry must outlive the registrations; StudioSession owns
// them in that order.
void RegisterViewportTargetRpc(
    rpc::Dispatcher& dispatcher,
    ViewportTargetRegistry& registry);
} // namespace orbit::studio_session
