+++
path = "/editor/studio-ui/celestial-authoring"
title = "Celestial and System View panels"
kind = "subsystem"
status = "stable"
summary = """
CelestialAuthoringUi (the Celestial panel and the 'Celestial Tools' Properties provider) adds systems, bodies, reference nodes, capabilities, ring bands, \
atmosphere presets/solves and seeded systems; SystemViewUi (the System View panel) shows a 2D orbit canvas with the simulation clock and drag handles for \
periapsis/apoapsis and reference-node position. Both are thin presentation over editor_model services and CommandService; this block lists which \
operations are reachable over RPC/MCP and which are not."""
owner_module = "OrbitStudioUi"
keywords = ["celestial panel", "system view", "orbit gizmo", "periapsis", "apoapsis", "reference node", "barycenter", "ring band", "capability", "atmosphere preset", "solve", "seeded system", "generate system", "check model", "celestial.set_capability", "atmosphere.solve", "orbit canvas"]
sources = [
  "engine/studio_ui/src/CelestialAuthoringUi.cpp",
  "engine/studio_ui/include/orbit/studio_ui/CelestialAuthoringUi.hpp",
  "engine/studio_ui/src/SystemViewUi.cpp",
  "engine/studio_ui/include/orbit/studio_ui/SystemViewUi.hpp",
  "engine/editor_model/src/CelestialAuthoringModel.cpp",
  "engine/editor_model/src/SystemViewModel.cpp",
  "engine/editor_rpc/src/EditorRpcService.cpp",
  "tools/mcp_server/orbit_editor_mcp_server.py",
]
symbols = [
  "CelestialAuthoringUi",
  "RelevantToSelection",
  "SystemViewUi",
  "GizmoDragKind",
  "SystemCanvasProjection",
  "ApplyAnalyticOrbitEdit",
  "SetReferenceNodePosition",
  "SetCapabilityEnabled",
  "RequireAtmosphereCapability",
  "orbit_atmosphere_solve",
]
invariants = [
  "The Celestial panel and the 'Celestial Tools' provider (id orbit.celestial-authoring) share ONE draw path, CelestialAuthoringUi::Draw; the provider is shown when RelevantToSelection() is true (selection is a system, body, reference node, ring band or capability). Add controls there, not in a second code path.",
  "Neither panel writes the ObjectStore. Every authored change is made through CelestialAuthoringModel, SystemViewModel, CelestialRecipeService, AtmospherePropertySolver or EnsureAllPlanetSurfaces, all built over world.Commands() (CommandService); the one direct call is SetProperty(kAtmosphereAuthoringMode) for the Derived Composition / Expert Coefficients buttons. The panels keep only presentation buffers (names, seed, drafts, status line).",
  "Models are rebuilt every frame from session_.World(). With no world open both panels print a prompt and return; SystemViewUi also drops its selected system, focus, orbit/reference drafts and active gizmo so nothing stale survives a world switch.",
  "Multi-step operations are single undo steps: Generate Seeded System runs inside the 'Generate Celestial System Recipe' transaction; an orbit-handle or Apply Orbit Edit commit runs in the 'Manipulate Analytic Orbit' transaction (eight SetProperty calls, rolled back on failure).",
  "Gizmo drags are drafts: periapsis/apoapsis and reference-node handles edit orbitDraft_/referenceDraft_ while the left button is down and commit exactly once on leftReleased. The clock is paused when a handle is grabbed.",
  "Direct orbit editing needs the 'Analytic Conic' capability model (ApplyAnalyticOrbitEdit throws otherwise); the P/A handles are drawn only for the single selected body whose analytic eccentricity is below 1 - 1e-10. Failures land in the panel status line, never as an unhandled exception.",
  "Every button handler catches std::exception into status_; a failed action leaves the world as it was because the model/service transactions roll back.",
  "Generate Seeded System and Check Model call world.RebuildUniverse() immediately; other edits reach the universe through the per-frame StudioSession::Tick, which calls RefreshUniverseIfChanged (rebuild when the ObjectStore revision differs from the universe source revision).",
  "The System View time controls (Playing, Rate, Time (us), +/-1 min, +/-1 h, System Epoch) act on session_.Clock(), the same clock the Simulation transport and time.get/time.set/time.step drive; typing a time or pressing a step button pauses the clock first.",
  "RPC/MCP parity is partial: the operations listed under 'Known gaps' have no dedicated RPC or MCP tool. A new panel action must ship its RPC method, MCP tool and docs/ORBIT_MCP.md row in the same change (AGENTS.md).",
]
related = ["/editor/model", "/editor/mcp-rpc", "/editor/session", "/rendering/atmosphere/authoring-solver", "/celestial/orbits", "/celestial/rings", "/authoring/commands"]
depends_on = ["/editor/model", "/authoring/commands", "/editor/session", "/rendering/atmosphere/authoring-solver"]
used_by = ["/editor/studio-ui"]
verify = [
  "ctest -R Orbit.CelestialAuthoringModel",
  "ctest -R Orbit.SystemViewModel",
  "ctest -R Orbit.CelestialRecipeService",
  "ctest -R Orbit.AtmospherePropertySolver",
  "ctest -R Orbit.EditorRpc",
]
verified = "55d48117"

[routes]
"atmosphere preset or Solve gives conflicts or wrong coefficients" = "/rendering/atmosphere/authoring-solver"
"orbit shape, ephemeris or trajectory is wrong rather than the panel" = "/celestial/orbits"
"ring bands render or bind wrongly" = "/celestial/rings"
"recipe defaults (star, rocky planet, moon, seeded system) look wrong" = "/editor/model"

[[diagnose]]
symptom = "Celestial panel shows 'Open a world to author celestial systems.' or System View shows 'Open a world to inspect a celestial system.'"
steps = [
  "Call world.active (orbit_world_active): `open` must be true; if not, world.open a world from world.list.",
  "A project can be open with no world; Draw returns early on world.HasWorld() == false in both panels.",
]
docs = ["/editor/studio-ui/world-and-project"]

[[diagnose]]
symptom = "System View says 'No celestial systems are authored in this world.'"
steps = [
  "SystemViewModel::Systems() found no kCelestialSystemType object. Call object.roots then object.children on the World root.",
  "Create one: Celestial panel 'Add to World' > + Add System, or celestial.create_from_recipe / object.create (type celestial system, parent = World root).",
]

[[diagnose]]
symptom = "Atmosphere section is missing from the Celestial panel"
steps = [
  "The section only appears when the selection is an atmosphere capability object or the selected body has an atmosphere capability child.",
  "Call celestial.capabilities (orbit_celestial_capabilities) for the body: state 'absent' means none; enable it with celestial.set_capability.",
  "The Atmosphere section's 'Derived authority / Explicit / Imported' counts cover ten atmosphere properties (top radius, Rayleigh, Mie and absorption coefficients and heights).",
]
docs = ["/rendering/atmosphere/authoring-solver"]

[[diagnose]]
symptom = "Solve or a preset reports conflicts or derives nothing"
steps = [
  "Read the status line, then reproduce with atmosphere.solve / atmosphere.apply_preset on the atmosphere capability object id (not the body id) and read events[] (derived, no_change, conflict, invalid_input).",
  "Conflicts mean Explicit/Imported/Locked authority was preserved; switch authority or clear the property in Properties before solving.",
  "Expert Coefficients mode performs no derivation, so Solve has nothing to write there.",
]
docs = ["/rendering/atmosphere/authoring-solver"]

[[diagnose]]
symptom = "Orbit P/A handles do not appear, or Apply Orbit Edit fails"
steps = [
  "Exactly one object must be selected (selection.get) and it must be a celestial body with an Orbit capability; with several selected the manipulator is reset.",
  "Read the capability with object.get: kCapabilityModel must be 'Analytic Conic'. Other models (ephemeris, N-body, fixed) throw 'Direct orbit manipulation requires the Analytic Conic model.'",
  "The handles only exist for eccentricity below 1 - 1e-10; open or parabolic orbits can only be edited through the numeric fields.",
  "A parabolic edit needs positive periapsis distance; an elliptic or hyperbolic edit needs positive semi-major axis, finite values, e >= 0 and mu > 0.",
]
docs = ["/celestial/orbits"]

[[diagnose]]
symptom = "Bodies do not move in System View"
steps = [
  "Call time.get (orbit_time_get): `playing` false means the clock is paused (typing a time or a step button pauses it).",
  "time.set {playing: true} or adjust rate; the same clock drives the viewport, sun and atmosphere.",
  "The System Epoch button resets the clock to kSystemEpochMicroseconds of the selected system (0 when unset).",
]

[[diagnose]]
symptom = "'+ <capability>' button reports 'already exists on this body' or 'Select a celestial body or one of its capability children first'"
steps = [
  "Singleton capabilities are refused by CelestialAuthoringModel::AddCapability when one is already a child of the body; celestial.capabilities shows the state.",
  "To re-enable a disabled one use celestial.set_capability (it flips kCapabilityEnabled and only creates the object when none exists).",
]

[[diagnose]]
symptom = "New object does not show up in the viewport or System View until later"
steps = [
  "Edits become visible when the universe is rebuilt: Generate Seeded System and Check Model do it immediately, everything else on the next StudioSession::Tick.",
  "Press Check Model (Validation section): it lists semantic errors (system not directly under World, body under a non-structural parent, duplicate singleton capability, ring band not under a Ring System) and then rebuilds the runtime composition; a throw is shown as '[error] Runtime composition: ...'.",
]
+++

## What each panel does

| Panel | Class (kPanel) | Sections and actions |
| --- | --- | --- |
| Celestial (right dock, closed by default) and Properties provider 'Celestial Tools' | `CelestialAuthoringUi` | Edit Selection: `+ <capability>` per `AvailableCapabilities()`, `+ Ring Band` (selection is a Ring System), Remove Selected Ring Band, Remove Selected Capability. Atmosphere (selection is or contains an atmosphere capability): preset buttons, Derived Composition / Expert Coefficients, Solve Derived Coefficients. Add to World: Give All Planets a Surface, + Add System, + Add Body, + Add Reference / Barycenter. Generate Seeded System: seed, rocky planet count, moons, names. Validation: Check Model. |
| System View (centre dock, closed by default) | `SystemViewUi` | System list, Time block, Frame All / Zoom +/-, a canvas of the selected system with 128-sample trajectories per body, click-to-select (14 px), double-click to focus, P/A periapsis/apoapsis handles (12 px pick radius), reference-node drag, then numeric Analytic Orbit Manipulator and Reference Node Manipulator forms. |

## Calls behind the buttons

| Action | Call | Notes |
| --- | --- | --- |
| Add System / Body / Reference, `+ capability`, ring band, remove | `CelestialAuthoringModel::CreateSystem/CreateBody/CreateReferenceNode/AddCapability/AddRingBand/RemoveSelected*` | `CreateBody` also runs `EnsurePlanetSurface`; capability removal deletes provenance children first inside 'Remove Celestial Capability' and refuses other children. |
| Give All Planets a Surface | `editor_model::EnsureAllPlanetSurfaces` | one 'Give Planets a Surface' transaction |
| Presets, Solve | `AtmospherePropertySolver::ApplyPreset / Solve` | own their transaction only when none is active |
| Generate Seeded System | `CelestialRecipeService::CreateSeededSystem` under the World root, then select the system and `RebuildUniverse()` | seed must be non-negative, planet count 1..uint32 max |
| Orbit and reference edits | `SystemViewModel::ApplyAnalyticOrbitEdit / SetReferenceNodePosition` | reference position is a single SetProperty |

## RPC / MCP parity

Reachable with a dedicated method and MCP tool (`engine/editor_rpc/src/EditorRpcService.cpp`, `tools/mcp_server/orbit_editor_mcp_server.py`):

| Panel operation | RPC | MCP tool |
| --- | --- | --- |
| Give All Planets a Surface | `world.ensure_planet_surfaces` | `orbit_world_ensure_planet_surfaces` |
| Create a star, rocky planet or moon from the physical recipes | `celestial.create_from_recipe` | `orbit_celestial_create_from_recipe` |
| List / enable / disable a body's capabilities | `celestial.capabilities`, `celestial.set_capability` | `orbit_celestial_capabilities`, `orbit_celestial_set_capability` |
| Atmosphere presets, Solve | `atmosphere.presets`, `atmosphere.apply_preset`, `atmosphere.solve` | `orbit_atmosphere_presets`, `orbit_atmosphere_apply_preset`, `orbit_atmosphere_solve` |
| System View clock (Playing, Rate, Time, steps) | `time.get`, `time.set`, `time.step` | `orbit_time_get`, `orbit_time_set`, `orbit_time_step` |
| Create a celestial body under a parent | `body.create` (runs the shared kCreateCelestialBody command) | `orbit_body_create` |

`body.capabilities` / `body.set_capability` are a different, narrower pair: only the `surface.terrain` capability (`ProjectWorldRpc.cpp`). Use `celestial.*` for atmosphere, clouds, rings and the rest. `atmosphere.*` take the atmosphere capability object's id, which `celestial.capabilities` does not return; find it with `object.children` on the body.

Generic methods reach the same CommandService (`object.create`, `object.delete`, `property.set`, `transaction.begin/commit`, `selection.set`), so the lower-level effect of every button below is scriptable, but none has a dedicated celestial method or MCP tool.

## Known gaps

Verified by searching every `.name = "..."` registration under `engine/` and `apps/` and every tool in `orbit_editor_mcp_server.py`:

- Generate Seeded System: `CreateSeededSystem` is called only from the panel. `celestial.create_from_recipe` makes a single star, rocky planet or moon, not a seeded system.
- Check Model (`CelestialAuthoringModel::Validate` plus runtime rebuild): no RPC or MCP tool returns these diagnostics.
- Orbit and reference-node manipulators: no method for `ApplyAnalyticOrbitEdit` or `SetReferenceNodePosition`; scripted equivalents are `property.set` calls (wrap them in `transaction.begin/commit` to match the single undo step).
- Add System, Add Reference / Barycenter, `+ Ring Band`, Remove Selected Capability / Ring Band: only generic `object.create` / `object.delete` exist, without the model's parent and singleton checks.
- Atmosphere authoring mode buttons: only generic `property.set` on `kAtmosphereAuthoringMode`.
- System View canvas state (selected system, focus, zoom, Frame All): presentation only, no method.

When closing one of these, extract the model call the button makes, register the RPC, add the MCP tool and the `docs/ORBIT_MCP.md` row in the same change, then delete the bullet here.
