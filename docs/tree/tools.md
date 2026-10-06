+++
path = "/tools"
title = "Tools, probes and automation"
kind = "section"
status = "stable"
summary = """
Development-time infrastructure: the sandbox dev server (loopback line protocol), the hot-reload probe and the eye-adaptation reloadable module, and the Python tooling \
under tools/ (MCP servers for Studio and the docs, validators, benchmarks). Studio's own RPC/MCP automation is documented under /editor/mcp-rpc."""
keywords = ["tools", "dev server", "probe", "mcp server", "python tools", "benchmark", "validator"]
related = ["/editor/mcp-rpc", "/docs-system", "/foundation/hot-reload"]

[routes]
"sandbox automation on port 4319" = "dev-server"
"prove the hot-reload path, ABI and state migration" = "hot-reload-probe"
"a real reloadable module (eye adaptation)" = "eye-adaptation-module"
"Studio MCP server, RPC methods" = "/editor/mcp-rpc"
"docs MCP server, docs CLI" = "/docs-system"
+++

`tools/` contains: `mcp_server/orbit_editor_mcp_server.py` (Studio over JSON-RPC, port 4320), `mcp_server/orbit_mcp_server.py` (sandbox dev server, port 4319),
`mcp_server/orbit_docs_mcp_server.py` (this documentation tree), `orbit_docs_cli.py`, `render_terrain_survey.py` and
`TerrainBenchmark.cpp` (a manual survey that is deliberately not a CTest gate).
