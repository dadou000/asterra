+++
path = "/foundation/rpc"
title = "JSON-RPC core (Value, Error, Dispatcher)"
kind = "subsystem"
status = "stable"
summary = "The transport-independent JSON-RPC 2.0 building blocks: a JSON Value, rpc::Error with codes, MethodDescriptor (name, description, mutating flag) and the Dispatcher that registers handlers; Studio's authoring API, view RPC, shading RPC and profiler RPC all register on it."
owner_module = "OrbitRpc"
keywords = ["rpc", "json rpc", "dispatcher", "method descriptor", "value", "error code", "register", "transport"]
sources = [
  "engine/rpc/include/orbit/rpc/JsonRpc.hpp",
  "engine/rpc/CMakeLists.txt",
]
symbols = ["Value"]
invariants = [
  "The dispatcher is transport independent: the loopback socket server and tests drive the same registered methods.",
  "Mutating methods are flagged in their MethodDescriptor; handlers validate parameters and fail with rpc::Error and an actionable message (/editor/mcp-rpc).",
]
related = ["/editor/mcp-rpc", "/tools/dev-server"]
depends_on = ["/foundation/core"]
used_by = ["/editor/reports", "/editor/studio-session", "/rendering/shading"]
verify = [
  "ctest -R Orbit.Rpc",
]
verified = "b0a0de7f"
+++


