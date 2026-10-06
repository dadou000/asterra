+++
path = "/authoring/scene"
title = "Semantic scene (ObjectStore)"
kind = "subsystem"
status = "stable"
summary = "ObjectStore is the persistent semantic scene: object records and their explicitly stored schema properties on the world's SQLite connection. Mutations need a capability key that only the command layer can create."
owner_module = "OrbitScene"
keywords = ["scene", "object store", "semantic", "object record", "mutation key", "revision", "transaction", "sqlite"]
sources = [
  "engine/scene/include/orbit/scene/ObjectStore.hpp",
  "engine/scene/CMakeLists.txt",
]
symbols = ["ObjectRecord"]
invariants = [
  "Persistent scene state can be mutated only through the command layer: mutation methods require a MutationKey that only CommandService can construct, so panels, plugins and MCP adapters cannot reach around validation and undo.",
  "Explicit editor transactions share the scene's SQLite connection, so multi-command commits are atomic on disk, not merely grouped in undo history.",
  "The semantic revision is monotonic; inside an explicit transaction one revision is published only when the transaction commits and rolled-back transactions do not advance it.",
]
related = ["/authoring/commands", "/authoring/documents", "/authoring/schema", "/rules/placement"]
depends_on = ["/authoring/documents", "/authoring/schema", "/foundation/core"]
used_by = ["/apps/player", "/apps/studio", "/authoring/commands", "/authoring/cooked-project", "/authoring/plugins", "/authoring/selection", "/editor/model", "/editor/session", "/rendering/volumes/fields", "/rendering/volumes/render", "/rendering/volumes/representation", "/rendering/volumes/solver", "/world/path-geometry", "/world/paths", "/world/surface-composition", "/world/world-model"]
verify = [
  "ctest -R Orbit.AuthoringModel",
]
verified = "b0a0de7f"
+++

Rebuild systems (terrain, surface, proxies) key off the semantic revision.
