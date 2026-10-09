#pragma once

#include <orbit/rpc/JsonRpc.hpp>

namespace orbit::studio_session
{
class StudioSession;

// Registers the read-only terrain query surface scripts and agents use instead
// of steering a camera to read the ground: terrain.list (terrain objects and
// their bodies), terrain.sample (batched elevation/climate/biome/tectonic
// samples at latitude/longitude points or along a transect) and
// studio.process_info (which process answers, so a client can tell a hot
// generation handoff or relaunch from the same process). The session must
// outlive the dispatcher registrations.
void RegisterStudioTerrainSampleRpc(
    rpc::Dispatcher& dispatcher,
    StudioSession& session);
} // namespace orbit::studio_session
