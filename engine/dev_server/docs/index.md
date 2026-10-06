+++
path = "/tools/dev-server"
title = "Dev server (loopback line protocol)"
kind = "subsystem"
status = "stable"
summary = "A loopback-only, non-blocking, line-framed transport for local Orbit automation, polled once per frame from the main thread; the sandbox serves it on port 4319 and the older text commands remain while clients migrate to JSON-RPC."
owner_module = "OrbitDevServer"
keywords = ["dev server", "loopback", "4319", "line protocol", "sandbox automation", "legacy text commands", "orbit_mcp_server"]
sources = [
  "engine/dev_server/include/orbit/dev_server/DevServer.hpp",
  "engine/dev_server/CMakeLists.txt",
]
symbols = ["DevServerConfig"]
invariants = [
  "Bound to 127.0.0.1 only; never reachable from the network.",
  "Polled from the main thread once per frame and never blocks, so a missing or slow client cannot stall the render loop.",
  "The transport is protocol agnostic through a MessageHandler; each message and response is one UTF-8 line.",
]
related = ["/editor/mcp-rpc", "/apps/sandbox", "/foundation/rpc"]
depends_on = ["/foundation/core"]
used_by = ["/apps/sandbox", "/apps/studio"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++

The Studio automation endpoint is separate: JSON-RPC on port 4320 (`/editor/mcp-rpc`); `tools/mcp_server/orbit_mcp_server.py` bridges this older sandbox protocol.
