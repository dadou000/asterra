+++
path = "/tools/dev-server"
title = "Dev server (loopback line protocol)"
kind = "subsystem"
status = "stable"
summary = "A loopback-only, non-blocking, line-framed transport for local Orbit automation, polled once per frame from the main thread; Studio serves its JSON-RPC endpoint (port 4320) over it. The standalone sandbox app and its port-4319 text protocol were removed in 0.0.9."
owner_module = "OrbitDevServer"
keywords = ["dev server", "loopback", "line protocol", "json-rpc transport"]
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
related = ["/editor/mcp-rpc", "/foundation/rpc"]
depends_on = ["/foundation/core"]
used_by = ["/apps/studio"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++

The Studio automation endpoint is JSON-RPC on port 4320 (`/editor/mcp-rpc`).
