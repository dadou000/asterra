+++
path = "/docs-system"
title = "Orbit docs system (blocks, tree, tools)"
kind = "meta"
status = "stable"
summary = """
Documentation is a tree of small Markdown blocks with TOML front matter (summary, invariants, source and symbol links, \
routing hints, diagnosis playbooks). A read-only MCP server and CLI (tools/orbit_docs, tools/mcp_server/orbit_docs_mcp_server.py) \
serve the tree offline; a checker keeps links, sources and symbols honest in CI."""
keywords = ["docs", "documentation", "block", "front matter", "tree", "mcp", "orbit_docs", "checker", "schema", "how to write"]
sources = [
  "tools/orbit_docs/index.py",
  "tools/orbit_docs/api.py",
  "tools/orbit_docs_cli.py",
  "tools/mcp_server/orbit_docs_mcp_server.py",
]
symbols = ["DocsIndex", "DocsApi", "Block"]
invariants = [
  "Every node has a path, a title and a summary; the summary is what its parent shows, so it must stand alone.",
  "Every non-root node's parent path exists; nodes are reached by descending from /.",
  "sources, symbols and links are validated by `python tools/orbit_docs_cli.py check`; broken ones fail CI.",
  "Blocks live next to the code they describe (engine/<module>/docs/) or under docs/tree/ for cross-cutting nodes; the tree path in front matter, not the directory, decides where a block appears.",
  "A block is one thing an engineer might ask a question about: aim for 200-800 words; split above ~1200.",
  "Files without front matter stay indexed as /legacy/<name>, one child per `##` section; migrate by adding front matter, not by copying text.",
]
related = ["/rules/completion-check"]
used_by = ["/rules/completion-check"]
verify = ["python tools/orbit_docs_cli.py check", "python -m unittest discover -s tools/tests -p 'test_orbit_docs.py'"]
verified = "00d8c5b6"

+++

## Tools

`tools/mcp_server/orbit_docs_mcp_server.py` (read-only, offline, no Studio needed):

| MCP tool | Use |
| --- | --- |
| `orbit_docs_root` | start here: engine summary, sections, routing |
| `orbit_docs_for_task(task)` | one packet for a task described in words: best nodes with invariants, routes, playbooks, verification, sources, plus always-applicable rules |
| `orbit_docs_get(path, section?, offset?, max_chars?)` | read a node; `section` = one heading of the body |
| `orbit_docs_children(path)` | cheap navigation |
| `orbit_docs_search(query)` | ranked search over titles, keywords, symptoms, symbols, invariants, text |
| `orbit_docs_for_target(file_or_symbol)` | impact analysis before editing: owning blocks, invariants, depends/used-by, applicable rules |
| `orbit_docs_coverage` | engine/apps modules with no card and documents nothing links to |
| `orbit_docs_check` | validate the tree |

Register: `claude mcp add orbit-docs -- python <repo>/tools/mcp_server/orbit_docs_mcp_server.py`.
The same operations exist in `python tools/orbit_docs_cli.py {check,tree,get,search,task,target,stale,coverage,scaffold}`.

Every response uses one envelope: `ok`, `operation`, `request_id`, `duration_ms`, `result` / `error`
(`code`, `message`, details), `warnings`, `suggestions`. Errors carry stable codes
(`DOC_NOT_FOUND` with `did_you_mean`, `SECTION_NOT_FOUND` with `available`, `EMPTY_QUERY`).

## Format

A block is `<name>.md` with TOML front matter between `+++` lines, then Markdown.

```toml
+++
path = "/rendering/terrain/clipmaps/level-planning"   # required, absolute tree path
title = "Level planning"                               # required
kind = "concept"          # root|section|subsystem|concept|rule|playbook|reference|placement|meta
status = "stable"         # stable|experimental|planned|draft|historical
summary = "One or two sentences that stand alone."     # required
owner_module = "OrbitTerrainView"
keywords = ["planner", "lod"]                          # words people will search with
sources = ["engine/terrain_view/src/ClipmapPlanner.cpp"]   # must exist
symbols = ["ClipmapPlanner"]                           # must appear in the sources
invariants = ["What must stay true during a refactor."]
related = ["/other/node"]                              # links that must exist
depends_on = ["/other/node"]                           # this relies on that
used_by = ["/other/node"]                              # that relies on this
verify = ["How to prove the invariants still hold."]
verified = "00d8c5b6"                                  # commit the facts were checked against
applies_to = ["engine/**"]                             # rules: globs of files they govern
include_in_tasks = false                               # rules: always listed by docs.for_task

[routes]                                               # "if you are here because of X, go to Y"
"wrong detail level" = "level-planning"                # child name or absolute path

[[diagnose]]                                           # executable-by-hand playbooks
symptom = "terrain too coarse near the camera"
steps = ["Read ...", "Switch on ..."]
docs = ["/rendering/terrain/clipmaps/level-planning"]
+++
```

### Rules of thumb

- **One block = one question.** `sinking`, `phase-alignment`, `level-selection`: yes. A 40-page terrain
  document or a block per variable: no.
- **Invariants over narrative.** Put what must stay true in `invariants`; keep history in the body.
- **Routing at the parents.** Every section/subsystem node should say where each symptom goes (`[routes]`).
- **Diagnose with instruments that exist.** Playbook steps name real overlays, RPC methods or numbers.
- **Set `verified`** to the commit you checked the block against; `python tools/orbit_docs_cli.py stale`
  lists blocks whose `sources` changed since then.
- **Do not invent.** A step or invariant you have not checked against code or a measurement does not belong in a block.

## Module cards and coverage

Every directory under `engine/` and `apps/` must have a card: a block that lives in `<module>/docs/` (or names the module's files in `sources`).
`python tools/orbit_docs_cli.py coverage --require-modules` fails when one is missing and CI runs it. To add a module:

1. `python tools/orbit_docs_cli.py scaffold engine/my_module --path /rendering/my-module > engine/my_module/docs/index.md`
   (creates a draft from the public headers, top-level types, CMake dependencies mapped to existing cards and registered test names).
2. Replace every `TODO(docs)` (check rejects a block that still has one): write the summary and keywords, then invariants **only from what you verified**
   in code, specs or tests, then route the parent section to the card and set `verified`.

Dependency edges (`depends_on` / `used_by`) in module cards follow the CMake link graph; keep them in step when CMake changes.

## Migrating a long document

1. Check how it is indexed: `python tools/orbit_docs_cli.py tree /legacy --all --depth 2`.
2. Create blocks for the sections people actually ask about, in the owning module's `docs/` directory, with
   front matter and a tree path under the right section.
3. Replace the moved text in the old file with a pointer to the new path (or delete it) so there is one source.
