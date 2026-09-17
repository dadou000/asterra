# Orbit V0.0.3 — World & Celestial Authoring Scaffold

Status: **active implementation**

This document is a normative supplement to `V0.0.3_SPEC.md`. It fills the gap between the generic Orbit Studio shell and a complete user-facing workflow for creating and configuring projects, worlds, celestial systems and celestial bodies.

The implementation must keep the existing V0.0.3 invariants: no editor-only mutation path, no renderer authority, no generated geometry as source of truth, and no duplicate world model for UI/MCP/plugins.

---

## WA0 — Scope and design rule

Orbit Studio should make world setup feel like ordinary object authoring rather than a modal wizard.

The default workflow is:

1. Create or open a project.
2. Create/select an authoritative `.orbitworld` document.
3. Use Explorer to create/select a celestial system.
4. Create/select bodies under that system.
5. Configure body properties and capabilities through the shared schema-driven Properties panel.
6. Observe dependency invalidation/build status in-place.
7. Preview the active body through one or more `RenderView` panels.
8. Use the same commands from Studio, plugins and MCP.

World setup remains contextual and dockable. Large one-shot setup wizards are not the authority.

---

## WA1 — Project and world document workflow

### Required project UI

- New Project
- Open Project
- Recent Projects
- Save Project
- Close Project
- Project Settings

### Required world document UI

- list project worlds;
- create world;
- rename world display name;
- choose startup world;
- open/switch active authoring world;
- validate world schema/migrations;
- show authoritative file path and world ID.

### Permanent backend contract

`ProjectDocument` owns project-level world discovery and creation. World documents live below `Worlds/` and use `.orbitworld` SQLite authority. The editor must never silently overwrite an existing world document.

Implemented foundation:

- `ProjectDocument::WorldPaths()` enumerates authoritative project worlds;
- `ProjectDocument::CreateWorld()` creates an additional world safely;
- `ProjectDocument::SetStartupWorld()` validates and atomically persists the startup-world choice.

Remaining UI work:

- Studio project browser/start page;
- hot project/session switching;
- dockable World Documents panel;
- recent-project persistence.

---

## WA2 — Semantic universe hierarchy

The Explorer hierarchy is authoritative semantic state, not render state.

Recommended shape:

```text
World
  Celestial System
    Star / body records
    Rocky body
      Surface-related authored children
      Path Networks
      Decals
      Gameplay/world objects
    Moon
```

Generated terrain chunks, GPU resources, road mesh segments, lane meshes and field pages do not become persistent Explorer children.

Implemented foundation:

- `World` schema;
- `Celestial System` schema;
- `Celestial Body` schema;
- shared `ObjectStore` hierarchy;
- Explorer search/selection/rename/reparent;
- transactional undo/redo.

---

## WA3 — Contextual creation commands

Creation must use the shared command registry.

Required commands:

- Create Celestial System
- Create Celestial Body
- duplicate body/system where schema-compatible;
- delete with dependency validation;
- future capability add/remove commands.

Implemented foundation:

- `Create Celestial System` is enabled for one selected World;
- `Create Celestial Body` is enabled for one selected World or Celestial System;
- selecting a World can create/reuse a system before creating a body;
- commands create persistent semantic objects transactionally;
- new objects become the shared selection;
- command metadata contributes to Explorer context and Properties toolbar surfaces.

Acceptance:

- Studio, plugin and automation invocation reach the same command implementation;
- undo removes the complete semantic creation transaction;
- redo recreates equivalent object/property state.

---

## WA4 — Body property authoring

The normal Properties panel remains schema-driven.

Baseline V0.0.3 body properties:

### Shape

- ellipsoid enabled;
- equatorial/reference radius;
- polar radius.

### Physical

- mass;
- parent-frame position.

### Rotation

- rotation period;
- axial tilt;
- phase at epoch.

### Rendering/content

- material asset.

Implemented foundation:

- all properties above exist in the shared world schema;
- Properties edits use `CommandService`;
- multi-object compatible edits are supported;
- validation ranges and units are schema metadata.

Next expansion should add capability records rather than bloating the core body schema with terrain-specific fields.

---

## WA5 — Body capabilities

Body capabilities are composition, not subclasses.

The UI must expose capability add/remove/configure operations for real implemented capabilities only.

Expected families as they become production-ready:

- shape/reference surface;
- gravity;
- rotation;
- rocky surface/terrain;
- hydrosphere;
- atmosphere;
- field sets;
- future specialized body capabilities.

A body without terrain remains valid.

Capability panels should be generated from registered schema/capability metadata where practical. Custom editors are reserved for complex visualization rather than routine scalar settings.

---

## WA6 — Terrain/world-generation setup

For a terrain-capable rocky body, the authoring UI should expose semantic generator inputs and dependency status rather than direct GPU state.

Target groups:

- procedural recipe;
- world seed;
- global geomorph;
- tectonics/geology;
- elevation/hypsometry;
- erosion;
- hydrology;
- climate;
- soil;
- biomes;
- infrastructure/buildability fields.

The procedural dependency graph owns rebuild/invalidation. The editor shows status and requests work; it does not manually sequence generators.

Required preview modes eventually include:

- rendered surface;
- elevation;
- geology;
- drainage/hydrology;
- temperature;
- precipitation;
- biome;
- buildability/transport corridors.

---

## WA7 — Viewport and active-body workflow

World authoring must support more than one view without introducing singleton viewport assumptions.

Target workflow:

- active perspective body view;
- optional orthographic/body-map view;
- field/debug view;
- material preview;
- alternate body/frame view.

Each uses `RenderView` and the existing render graph.

Selecting a body should allow a contextual action to make it the target of a chosen viewport without changing project authority.

---

## WA8 — Project/world automation surface

The structured RPC/MCP surface should converge on:

```text
project.list_recent
project.open
project.save
project.validate

world.list
world.create
world.open
world.close
world.set_startup

body.list
body.create
body.capabilities
body.set_capability
```

Until dedicated RPC methods exist, generic command discovery/invocation remains the valid automation path for semantic system/body creation.

RPC must never bypass `CommandRegistry`, schema validation or authoritative project/world services.

---

## WA9 — Acceptance gate

World & Celestial Authoring is complete when a user can start from an empty project directory and, using Orbit Studio UI alone:

1. create/open the project;
2. create a second `.orbitworld` document;
3. choose its startup world;
4. create a celestial system;
5. create at least two bodies;
6. edit radius, mass, position, rotation and axial tilt through Properties;
7. save, close and reopen the project;
8. recover the same semantic hierarchy and settings;
9. undo/redo body/system creation and property edits;
10. perform equivalent body/system creation through MCP or a plugin command;
11. preview at least two bodies/views without editor-singleton assumptions.

Derived caches may be deleted before reopening; project/world authority must remain intact.

---

## Live implementation status — 2026-09-17

| Slice | Status | Notes |
|---|---|---|
| Project create/open backend | Implemented | `ProjectDocument` create/open and manifest persistence. |
| Multi-world project backend | Implemented | world enumeration, safe creation and startup-world switching added. |
| World/System/Body schemas | Implemented | shared world model owns the IDs and baseline properties. |
| System/body creation commands | Implemented | transactional contextual commands in the shared registry. |
| Explorer + schema Properties | Implemented | hierarchy editing, selection and common-property editing are in-tree. |
| Project browser UI | Planned | current Studio startup still opens command-line/local/scratch project. |
| World Documents panel | Planned | backend is ready; dockable UI not yet landed. |
| Hot active-world switching | Planned | requires session recomposition rather than rebinding scattered references. |
| Capability authoring UI | Planned | must be driven by real registered capabilities. |
| Terrain generator setup UI | Planned | dependency graph exists; user-facing setup surface remains to be built. |
| Dedicated world/body RPC domains | Partial | generic command/RPC path works; explicit convenience methods remain. |

This supplement should be updated alongside `V0.0.3_SPEC.md` until the world-authoring acceptance gate is closed.
