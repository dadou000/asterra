#pragma once

#include <orbit/rpc/JsonRpc.hpp>

namespace orbit::editor_rpc
{
// profiler.status / profiler.configure / profiler.capture: the RPC face of the
// always-on CPU micro-profiler (orbit/profiler/Profiler.hpp,
// docs/ORBIT_PROFILER.md).
void RegisterProfilerRpc(rpc::Dispatcher& dispatcher);
} // namespace orbit::editor_rpc
