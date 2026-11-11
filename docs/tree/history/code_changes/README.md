+++
path = "/rules/placement"
title = "Code change placement rules"
kind = "rule"
status = "stable"
summary = """
Before changing existing behaviour, write a short placement note in code_change/ that names the \
existing owner, the primary insertion point, the canonical state and what must not be duplicated. \
Extend the owner before creating a subsystem; UI initiates and owners execute; patch the narrowest \
existing seam; respect rebuild authority; keep UI and automation on one operation."""
keywords = ["placement", "owner", "insertion point", "duplicate", "state authority", "seam", "controller", "change note"]
include_in_tasks = true
applies_to = ["engine/**", "apps/**"]
invariants = [
  "Extend the class that already owns the state or behaviour before creating a second controller, cache, navigation state, renderer, command path or UI model.",
  "UI initiates; domain/state owners execute (UI -> command/service/owner -> canonical state -> renderer/presentation).",
  "Prefer an existing public method or command seam over a new parallel route; if none fits, add one to the current owner and route all callers through it.",
  "Before mutating an object, check whether it is rebuilt each frame from another source; if so change the upstream authoritative state, not the transient result.",
  "Expose one operation through the command registry / RPC seam; buttons and shortcuts invoke that same operation.",
  "A new file is justified only by a reusable concept with a clear owner, not by avoiding an edit to the file that already owns the responsibility.",
]
related = ["/rules/architecture", "/editor/mcp-rpc", "/editor/viewport/frame-selected"]
+++

# Code Change Placement Rules

This directory defines **where a requested change belongs inside existing Orbit code before implementation starts**.

The goal is to prevent feature work from creating duplicate systems, parallel state, ad-hoc helpers, or UI logic in the wrong layer.

## Required workflow

For every change that modifies existing behavior, first create or update a short placement note in this directory.

The note must answer these questions before code is written:

1. **User-visible behavior** — what changes from the user's point of view.
2. **Existing owner** — which current subsystem already owns that behavior or state.
3. **Primary insertion point** — the existing function/class/file where the new behavior should enter.
4. **State authority** — which existing state remains canonical after the change.
5. **Secondary touch points** — UI, commands, RPC, tests, serialization, renderer, etc. that need wiring but must not become new authorities.
6. **Explicit non-goals** — nearby systems that must not be duplicated or bypassed.
7. **Validation** — how to prove the change is integrated into the existing system rather than layered beside it.

## Placement rules

### 1. Extend the owner before creating a new subsystem

If Orbit already has a class that owns the relevant state or behavior, extend that class first.

Do not create a second controller, cache, navigation state, renderer, command path, or UI model merely because it is easier to implement in isolation.

### 2. UI initiates; domain/state owners execute

A toolbar button or panel may request an operation, but it should not become the authority for camera, world, terrain, renderer, simulation, or asset state.

Typical flow:

`UI -> command/service/owner -> canonical state -> renderer/presentation`

Avoid:

`UI -> directly mutate a presentation object that is rebuilt elsewhere`

### 3. Patch the narrowest existing seam

Prefer an existing public method or command seam over adding a new parallel route.

If no suitable seam exists, add one to the current owner and route all callers through it.

### 4. Respect refresh/rebuild authority

Before mutating an object, check whether it is reconstructed each frame or rebuilt from another state source.

If it is, modify the upstream authoritative state instead of the transient result.

### 5. Keep automation and UI on the same operation

When practical, expose one operation through the command registry / RPC seam and have buttons or shortcuts invoke that same operation. Do not implement a special UI-only version of important editor behavior.

### 6. Existing files are preferred when responsibility already exists there

A change should go into the existing implementation file that already owns the responsibility unless doing so would materially violate module boundaries or make the file unmaintainable.

A new file is justified when it introduces a reusable concept with a clear owner, not merely to avoid editing an existing file.

## Note format

Create one Markdown file per meaningful change using a short stable name, for example:

- `frame_selected_camera.md`
- `terrain_clipmap_resize.md`
- `runtime_blueprint_save.md`

Use this template:

```md
# <Change name>

## Behavior
<What the user expects.>

## Existing owner
- Module:
- Class/service:
- Canonical state:

## Primary insertion point
- File:
- Symbol/function:
- Reason:

## Secondary touch points
- File/symbol and purpose

## Must not be implemented in
- File/system and why

## Data/control flow
`A -> B -> C`

## Validation
- [ ] Existing authority remains canonical
- [ ] No duplicate state/controller was introduced
- [ ] UI/shortcut/automation reach the same operation where applicable
- [ ] Existing behavior still works
- [ ] New behavior is covered by tests or a deterministic smoke check
```

## Current example

See `frame_selected_camera.md` for the placement decision for the smooth 2-second camera framing tool.
