+++
path = "/rules/completion-check"
title = "Completion check"
kind = "rule"
status = "stable"
summary = """
Questions to answer before calling a code change complete: what happens when the implementation \
and the shader/script/content files are saved while Orbit runs, whether a failed patch keeps the \
active generation, whether state survives, whether old-generation threads/GPU work can still \
reference retired code, and whether a manual restart was introduced. Plus docs and MCP upkeep."""
keywords = ["done", "complete", "checklist", "definition of done", "finished", "review"]
sources = ["AGENTS.md"]
applies_to = ["engine/**", "apps/**", "cmake/**"]
include_in_tasks = true
invariants = [
  "If saving the implementation or its shader/script/content file does nothing until a manual rebuild, the feature violates the Orbit architecture.",
  "A failed patch must preserve the active generation.",
  "State survives the patch where users would expect it to (versioned migration if module-private state is required).",
  "No thread or GPU submission may still reference an old generation when it is retired.",
  "The change must not add a manual restart requirement or a separate launcher.",
]
related = ["/rules/hot-iteration", "/rules/placement", "/editor/mcp-rpc", "/docs-system"]
+++

From `AGENTS.md`, plus the documentation steps that keep the agent-facing tree trustworthy.

## Code

- [ ] What happens when the implementation file is saved while Orbit is running?
- [ ] What happens when its shader/script/content file is saved?
- [ ] Does a failed patch preserve the active generation?
- [ ] Does state survive the patch where users would expect it to?
- [ ] Can any thread/GPU submission still reference the old generation when it is retired?
- [ ] Did the change accidentally add a manual restart requirement or a separate launcher?

## Automation

- [ ] If the change adds a Studio capability: is it an RPC method and a dedicated MCP tool, documented
      in `docs/ORBIT_MCP.md` (`/editor/mcp-rpc`)? UI buttons call the same operation.

## Documentation

- [ ] Run `python tools/orbit_docs_cli.py target <changed file>`; are the invariants of the blocks
      it lists still true? Update them if not.
- [ ] If you changed behaviour a block describes, update the block and its `verified` commit.
- [ ] `python tools/orbit_docs_cli.py check` passes (CI runs it).
