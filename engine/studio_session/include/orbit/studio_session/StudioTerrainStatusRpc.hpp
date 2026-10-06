#pragma once

#include <orbit/rpc/JsonRpc.hpp>

namespace orbit::studio_session
{
class StudioSession;

// Registers read-only terrain service diagnostics (terrain.cache_stats): the
// same persistent GPU cache statistics the Surface authoring panel and the
// terrain cache overlay show. The session must outlive the dispatcher
// registrations; StudioSession registers these from its own constructor.
void RegisterStudioTerrainStatusRpc(
    rpc::Dispatcher& dispatcher,
    StudioSession& session);
} // namespace orbit::studio_session
