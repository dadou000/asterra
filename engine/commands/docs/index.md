+++
path = "/authoring/commands"
title = "Commands, transactions and undo"
kind = "subsystem"
status = "stable"
summary = "CommandRegistry catalogues named commands (parameters, choices, enablement, presentation hints); CommandService executes undoable edits and owns transactions. The UI, shortcuts and the command palette run registered commands, and RPC/MCP reach the same operations."
owner_module = "OrbitCommands"
keywords = ["command", "command registry", "undo", "redo", "transaction", "command palette", "enablement", "duplicate", "delete"]
sources = [
  "engine/commands/include/orbit/commands/CommandRegistry.hpp",
  "engine/commands/include/orbit/commands/CommandService.hpp",
  "engine/commands/CMakeLists.txt"]
symbols = ["CommandChoice", "CommandService"]
invariants = [
  "Every semantic mutation is a command with undo; there is no second mutation path (/rules/placement, /editor/mcp-rpc).",
  "Duplicate clones ONE leaf object with every explicitly stored property as a single undoable edit; hierarchy duplication stays explicit so a large subtree is never copied by accident.",
  "Delete of a leaf is undoable and restores the full object record and stored properties with the same stable ID.",
  "Removing a property override restores the schema default and undo restores the removed value.",
  "Presentation metadata (choices, asset filters, surface hints like 'explorer.context') is opaque to the command layer, which stays independent of the editor UI and the content subsystem; the command implementation remains authoritative.",
  "Transaction labels exposed to history UIs are presentation snapshots only; mutation remains exclusively Undo/Redo."]
related = ["/authoring/scene", "/editor/mcp-rpc", "/rules/placement"]
depends_on = ["/authoring/scene", "/authoring/schema", "/foundation/core"]
used_by = ["/apps/studio", "/authoring/plugins", "/editor/model", "/editor/session", "/rendering/lighting/radiance-cache", "/world/paths", "/world/surface-composition", "/world/world-model"]
verify = [
  "ctest -R Orbit.AuthoringModel",
  "ctest -R Orbit.CommandSurfaces"]
verified = "b0a0de7f"
+++


