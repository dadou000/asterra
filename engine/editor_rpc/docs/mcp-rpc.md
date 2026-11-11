+++
path = "/editor/mcp-rpc"
title = "MCP / RPC automation of Studio"
kind = "subsystem"
status = "stable"
owner_module = "OrbitEditorRpc"
summary = """
Studio serves newline-framed JSON-RPC 2.0 on 127.0.0.1:4320; tools/mcp_server/orbit_editor_mcp_server.py \
exposes each method as a thin MCP tool and holds no engine policy. Parity rule: anything a user can do \
in Studio must be reachable over RPC and MCP, by extracting the operation behind a button, registering an \
RPC method for it, adding a dedicated tool and documenting it - never by simulating input."""
keywords = ["mcp", "rpc", "json-rpc", "automation", "tool", "dispatcher", "parity", "agent", "port 4320", "register"]
sources = [
  "docs/ORBIT_MCP.md",
  "tools/mcp_server/orbit_editor_mcp_server.py",
  "engine/rpc/include/orbit/rpc/JsonRpc.hpp",
  "engine/editor_rpc/include/orbit/editor_rpc/EditorRpcService.hpp",
  "engine/studio_ui/src/StudioRenderViewRpc.cpp",
]
symbols = ["Dispatcher", "MethodDescriptor", "EditorRpcService", "orbit_rpc_call"]
applies_to = ["tools/mcp_server/**", "engine/editor_rpc/**"]
invariants = [
  "Anything reachable in Studio is reachable over RPC and MCP; if a step exists only as a UI click, extract the operation, make the button call it and register an RPC method for it.",
  "Do not drive Studio by simulating mouse or keyboard input; a missing RPC/MCP path is a gap to implement.",
  "The Python adapter holds no engine policy: every tool is a thin wrapper over one RPC method; orbit_rpc_call(method, params_json) reaches any method without a dedicated tool.",
  "Semantic mutations route through CommandService/CommandRegistry; project-document mutations through ProjectDocument.",
  "A new capability change touches four places in the same change: operation + RPC registration, dedicated MCP tool, docs/ORBIT_MCP.md coverage table, and this docs tree.",
  "Every mutating command shows an in-app toast (MCP: <method>); failed commands show a red toast.",
]
related = ["/rules/placement", "/rules/ui", "/legacy/orbit-mcp"]
used_by = ["/rendering/terrain/clipmaps/debugging"]
verify = [
  "python -m py_compile tools/mcp_server/orbit_editor_mcp_server.py (CI does this).",
  "ctest -R EditorRpc (engine/editor_rpc/tests) for registration and dispatch behaviour.",
]
verified = "04d589b3"

[routes]
"which RPC methods exist for views and terrain overlays" = "/legacy/orbit-mcp/panels-and-the-shading-tab"
"objects, bodies and properties" = "/legacy/orbit-mcp/objects-bodies-and-properties"
"profiler capture over MCP" = "/editor/profiler"
"run the atmosphere solver or apply an atmosphere preset" = "/rendering/atmosphere/authoring-solver"
"terrain GPU cache hit/miss/resident statistics" = "/rendering/terrain/gpu-cache"
+++

```text
MCP client -> tools/mcp_server/orbit_editor_mcp_server.py -> JSON-RPC 2.0 @ 127.0.0.1:4320
           -> Studio RPC dispatcher (engine/editor_rpc, apps/editor Main.cpp) -> commands/services
```

## Adding a capability (the four-step checklist)

1. **Extract the operation.** Put the logic behind a callable on the existing owner (see
   `/rules/placement`). The button, shortcut and RPC method call this same function.
2. **Register the RPC method.** Methods are registered on `rpc::Dispatcher` with a
   `MethodDescriptor { .name, .description, .mutating }` plus a handler, as in
   `engine/studio_ui/src/StudioRenderViewRpc.cpp` (`view.terrain_overlays_set`) and
   `engine/editor_rpc/src/EditorRpcService.cpp`. Validate parameters and throw `rpc::Error` with an
   actionable message; mutating methods set `.mutating = true`.
3. **Add a dedicated MCP tool** in `tools/mcp_server/orbit_editor_mcp_server.py` (a thin wrapper).
4. **Document it** in the coverage table of `docs/ORBIT_MCP.md` and in the relevant block of this tree.

## Connecting

`claude mcp add orbit-studio -- python <repo>/tools/mcp_server/orbit_editor_mcp_server.py`.
Override the endpoint with `ORBIT_RPC_HOST`, `ORBIT_RPC_PORT`, `ORBIT_RPC_TIMEOUT`. The adapter opens one
short connection per call so a Studio relaunch never leaves a dead socket. Studio must already be running.

The offline docs server is separate (`tools/mcp_server/orbit_docs_mcp_server.py`, see `/docs-system`)
and does not need Studio.
