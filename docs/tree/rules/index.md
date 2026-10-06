+++
path = "/rules"
title = "Rules"
kind = "section"
status = "stable"
summary = """
Normative rules for anyone (human or agent) changing Orbit: dependency/ownership architecture, \
save-to-reflect hot iteration, MCP/RPC parity, where a change must be placed, UI rules and the \
completion checklist. Read the ones that apply before editing."""
keywords = ["rules", "contract", "must", "normative", "agents", "contributor"]

[routes]
"modules, includes, ownership, dependency direction" = "architecture"
"does my change reload without a restart" = "hot-iteration"
"anything reachable in the UI must be reachable over RPC and MCP" = "/editor/mcp-rpc"
"where does my change belong, duplicate controller or state" = "placement"
"UI work, toolbar, progressive disclosure" = "ui"
"is my feature finished" = "completion-check"
+++

Rules are ordered by how often they bite:

1. `/rules/placement` — extend the existing owner of a behaviour; write the placement note first.
2. `/rules/hot-iteration` — every change needs an automatic reflection path.
3. `/editor/mcp-rpc` — UI capability and automation capability are the same operation.
4. `/rules/architecture` — one-way dependencies, no god object, no service locator.
5. `/rules/ui` — simple first, complete underneath.
6. `/rules/completion-check` — the questions to answer before calling a feature done.

Full source texts: `docs/ORBIT_ARCHITECTURE.md`, `docs/ORBIT_HOT_ITERATION.md`, `docs/ORBIT_UI_RULES.md`
(also reachable per section under `/legacy`) and `AGENTS.md`.
