+++
path = "/authoring"
title = "Authoring model"
kind = "section"
status = "stable"
summary = """
How a project is represented and edited: documents on disk, the schema/property registry, the semantic scene (ObjectStore) that only \
the command layer may mutate, commands with undo and transactions, selection, plugins, content/assets with a derived-data cache, \
and the cooked project the player loads. Studio, plugins, RPC and MCP are all clients of these same operations."""
keywords = ["authoring", "editor model", "project", "scene", "commands", "undo", "schema", "assets", "plugins"]
related = ["/rules/placement", "/editor/mcp-rpc"]

[routes]
"project file, worlds, .orbitworld, startup world, SQLite" = "documents"
"property definitions, units, defaults, validation" = "schema"
"semantic objects, revisions, atomic transactions" = "scene"
"undo/redo, commands, command palette, transactions" = "commands"
"selected objects" = "selection"
"Luau plugins, panels, validators, plugin manifest" = "plugins"
"assets, import, derived data cache, materials, decals, thumbnails" = "content"
"Windows texture decoding, live texture preview" = "content-wic"
"cooked project, player-side build output" = "cooked-project"
"RPC and MCP for all of this" = "/editor/mcp-rpc"
+++

The authority chain is: **documents (disk) -> ObjectStore (semantic scene) -> derived data** (terrain, meshes, caches).
Only `CommandService` can mutate the scene (it alone can create the `MutationKey`), which is what makes undo,
validation and MCP parity enforceable rather than conventional.
