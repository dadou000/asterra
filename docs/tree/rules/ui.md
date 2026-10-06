+++
path = "/rules/ui"
title = "UI rules"
kind = "rule"
status = "stable"
summary = """
Studio UI is simple first and complete underneath: contextual common actions, progressive \
disclosure for the rest, no artificial limits, one concept with many entry points (button, \
shortcut, palette, RPC/MCP), UI never owns state, and there is exactly one unified Studio \
application with no separate launchers."""
keywords = ["ui", "studio", "toolbar", "inspector", "progressive disclosure", "shortcut", "command palette", "launcher", "layout"]
sources = ["docs/ORBIT_UI_RULES.md", "engine/studio_ui/include/orbit/studio_ui/StudioViewportPanels.hpp"]
applies_to = ["engine/studio_ui/**", "engine/editor_ui/**", "apps/editor/**"]
invariants = [
  "UI initiates; domain/state owners execute. A button or panel never becomes the authority for camera, world, terrain, renderer or asset state.",
  "One concept, multiple entry points: toolbar, shortcut, command palette and RPC/MCP all call the same operation.",
  "The UI never imposes limits that do not exist in the engine (engine invariants are not UI restrictions).",
  "The Inspector is never a dead end; advanced controls stay reachable via inspectors, advanced sections, search, plugins, scripting or MCP.",
  "Prefer reversible actions; errors must be actionable.",
  "Keyboard shortcuts follow the user's layout, not physical key positions.",
  "There is one Studio application (Orbit.exe at the repository root); no launcher, project-manager app or alternate editor front end.",
]
related = ["/editor/mcp-rpc", "/rules/placement"]
+++

Source: `docs/ORBIT_UI_RULES.md` (30 numbered rules plus a UI review checklist), also readable per
section under `/legacy/orbit-ui-rules`.

## The tests to apply to a UI change

- **Common-case test:** is the frequent action one obvious step?
- **Expert-control test:** can an expert still reach every parameter, precisely and by keyboard?
- **Non-restriction rule:** did the UI add a limit the engine does not have?

For the structure of a UI-triggered operation (where the state lives, what the UI may touch)
see `/rules/placement` and its worked example `/editor/viewport/frame-selected`.
