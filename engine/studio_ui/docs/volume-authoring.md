+++
path = "/editor/studio-ui/volume-authoring"
title = "Volume authoring panel, field debug renderer and Shading tab panels"
kind = "subsystem"
status = "stable"
summary = """
VolumeAuthoringUi is the "Volumes" panel and the contextual "Volume Tools" inspector section: Volume presets, sources and \
effectors, rendering and live-solver controls (RegisterBase / DrawBase), plus the Representation/LOD policy, the M37 cache \
bake/import and the M38 particle/surface output controls (Register / Draw), all in VolumeAuthoringUi.cpp. \
SurfaceVolumeDebugRenderer draws the density/velocity/field-slice debug lines. ShadingUi is the two-panel Shading tab \
(browser plus editor/preview) over ShadingWorkspace. None of the Volume runtime controls has a dedicated RPC or MCP tool."""
owner_module = "OrbitStudioUi"
keywords = ["volume authoring", "volumes panel", "volume tools", "representation lod", "volume cache", "bake", "orbitvol", "particle output", "surface deposits", "field debug", "density slice", "field slice", "surface volume debug", "live solver", "local 3d", "M36", "M37", "M38", "shading tab", "shading panel", "shading materials", "ShadingUi", "shader preview panel"]
sources = [
  "engine/studio_ui/src/VolumeAuthoringUi.cpp",
  "engine/studio_ui/include/orbit/studio_ui/VolumeAuthoringUi.hpp",
  "engine/studio_ui/src/SurfaceVolumeDebugRenderer.cpp",
  "engine/studio_ui/include/orbit/studio_ui/SurfaceVolumeDebugRenderer.hpp",
  "engine/studio_ui/src/ShadingUi.cpp",
  "engine/studio_ui/include/orbit/studio_ui/ShadingUi.hpp",
  "engine/studio_ui/CMakeLists.txt",
  "engine/studio_ui/src/StudioViewportRenderer.cpp",
  "engine/editor_model/src/VolumeAuthoringCommands.cpp",
]
symbols = ["VolumeAuthoringUi", "DrawRepresentationPolicy", "RegisterBase", "DrawBase", "ContextInstance", "RelevantToSelection", "SurfaceVolumeDebugRenderer", "surfaceVolumeDebugRenderer_", "BakeVolumeCache", "VolumeCaches", "VolumeOutputRuntimeService", "RegisterVolumeCommands", "kCreateVolume", "ShadingUi", "TakePreviewResizeRequest", "ShadingWorkspace"]
invariants = [
  "VolumeAuthoringUi.cpp holds two layers: RegisterBase / DrawBase (the base workflow) and the real Register (panel 'Volumes', right dock, order 35, min 320x300) and Draw = DrawBase + DrawRepresentationPolicy. Extend the base workflow in RegisterBase / DrawBase and the representation, cache and output controls in Register / Draw.",
  "There is one authoring stack: the Studio-owned VolumeAuthoringUi registers itself as ContextInstance(), and the contextual provider 'orbit.volume-authoring' ('Volume Tools', order 120) calls the same Draw so it shares the field storage, solver, renderer, caches and status line. It is relevant only when exactly one object is selected and it is a Volume or a Volume Source/Effector whose parent is a Volume; StudioExpansionShell consults ContextInstance()->RelevantToSelection() so volume authoring wins over terrain ancestry.",
  "Authored values go through world.Commands().SetProperty with the panel's clamps (e.g. render steps 8-256, shadow steps 0-32, temporal weight 0-0.98, anisotropy +-0.95, solver resolution 8-1024, surface layers 1-32, output particle budget 1-1000000) and are reachable with the generic property.set. Structure goes through the command registry: kCreateVolume (preset: Empty, Smoke, Fire, Fog, Dust, Snow, Surface Flow), kRemoveVolume, kAddVolumeSource (kind), kAddVolumeEffector (kind), kMoveVolumeInputUp/Down, kRemoveVolumeInput, kPaintVolumeTerrainSource (position, radius, strength), registered by RegisterVolumeCommands and so visible to command.catalog / command.invoke.",
  "Runtime-only state is mutated directly, with no command, no undo and no persistence: renderer_->VolumeRenderSettings (temporal enable, render debug mode, follow target and object, Live/Passive distances and projected pixels, hysteresis, Coarse/Passive resolution and steps, region map), solver_->Settings (live, pause, step, reset, time step, dissipation, source scale, iterations, GPU budget, debug view, slice axis and index) and the VolumeCaches() registry. The Representation mode (Auto, Live, Coarse, Passive, Baked) itself is authored (kVolumeRepresentationMode).",
  "Changing a live-solver volume's resolution, surface layers, Local3D domain center or half extents sets settings.resetRequested, so the solver restarts; Live Solver controls appear only for the Surface2D5D and Local3D solver policies.",
  "Baked never masquerades as live data: forcing Baked with no attached validated cache resolves to Passive and the panel says so (diagnostics.bakedFallback). Bake & Attach attaches at once (resolution clamped 4-512, default 32; field mask = the volume's Density/Emission bits, else Density) and exports only when a path is set; Import attaches only after LoadVolumeCache validation succeeds; the panel shows IsVolumeCacheCurrent's reason when the attached cache is stale.",
  "The Particle / Surface Output controls only author requests: with output enabled and no attached cache the panel states that no synthetic events are emitted. Last-step and world-runtime counters come from VolumeOutputs().Latest and VolumeOutputRuntimeService().Diagnostics(), and the pending particle and surface request queues are shown but not drained here.",
  "SurfaceVolumeDebugRenderer is one alpha-blended, depth-less line-list draw into targets.color after the UniversalVolume passes: scalar fields draw a line of length min(value, 8) * scale along the slice axis, vector fields value * scale; the slice index is clamped to the axis resolution - 1. The 'SurfaceVolumeDebug' pass exists only when solverSettings.debugView is not Off and the wanted channel (Density, Velocity, or the chosen debugField for FieldSlice) is among the imported field channels, and Draw returns early with no resident tiles. Its HLSL is embedded in the .cpp and compiled in the constructor, which throws std::runtime_error if compilation fails.",
  "ShadingUi (panels 'Shading Materials', right dock, and 'Shading', center) owns presentation state only: every operation is a ShadingWorkspace call (the same one the shading.* RPC drives), failures are caught in Run and shown next to the controls, and a compile failure keeps the last working shader in the preview. The panel only requests a preview size; its owner takes it with TakePreviewResizeRequest and resizes the render view.",
]
related = ["/editor/studio-ui", "/editor/mcp-rpc", "/editor/ui-toolkit", "/editor/model", "/rendering/shading", "/rendering/volumes/solver", "/rendering/volumes/render", "/rendering/volumes/representation", "/rendering/volumes/fields"]
depends_on = ["/rendering/volumes/solver", "/rendering/volumes/render", "/rendering/volumes/representation", "/rendering/volumes/fields", "/rendering/shading"]
used_by = ["/editor/studio-ui"]
verify = [
  "ctest -R Orbit.VolumeAuthoringWorkflow",
  "ctest -R Orbit.Shading",
  "Select a Volume, force Baked with no cache, and confirm the panel reports the Passive fallback.",
]
verified = "55d48117"

[routes]
"how volumes are solved, rendered, cached or turned into particle requests" = "/rendering/volumes/solver"
"shader contract, parameters, templates or shading.* RPC" = "/rendering/shading"
"need to script a volume operation (cache, solver run, LOD)" = "/editor/mcp-rpc"

[[diagnose]]
symptom = "forced Baked volume looks like Passive or coarse"
steps = [
  "Read the panel's 'Resolved ... | previous ...' line and the muted text under it: 'Baked was forced but no validated M37 cache is attached' means the explicit Passive fallback.",
  "Under Volume Cache / Bake use Bake & Attach, or Import a .orbitvol; a failed import prints 'Import failed [status]: message'.",
  "When a cache is attached the line below the buttons says whether it matches the current authored inputs and bake settings, otherwise why it is stale.",
]
docs = ["/rendering/volumes/representation"]

[[diagnose]]
symptom = "Density Slice, Velocity Overlay or Field Slice shows nothing"
steps = [
  "Check the Live Solver 'Field Debug View' selection is not 'Off' and that the volume's solver policy is Surface2D5D or Local3D (the controls do not exist otherwise).",
  "Read the 'Residency {} | valid {} | pending {}' line: with zero resident tiles SurfaceVolumeDebugRenderer::Draw returns without drawing.",
  "The wanted channel (Density, Velocity or the chosen field) must be present in the volume's field mask; otherwise the SurfaceVolumeDebug pass is not added to the frame.",
  "Slice Index / Debug Layer above the domain is clamped to the last layer.",
]
docs = ["/rendering/volumes/solver", "/rendering/volumes/fields"]

[[diagnose]]
symptom = "particles or surface deposits do not spawn although output is enabled"
steps = [
  "If the panel says 'Output is enabled but this Volume has no CPU-readable field authority yet', bake or import a cache for the volume.",
  "Read 'Last step ... particles emitted/requested' and 'Candidates ... threshold rejects ... budget drops': raise Particle Budget / Step or lower Field Threshold accordingly.",
  "'World runtime: ... without readable authority' counts the volumes the runtime skips for the same reason.",
]
docs = ["/rendering/volumes/representation"]

[[diagnose]]
symptom = "Shading preview does not reflect my shader edit"
steps = [
  "Read the status under the source editor: 'Compile failed: the preview keeps the last working shader.' followed by diagnostics, or 'Compiled OK in N ms'.",
  "Without Live compile checked, use 'Save and compile'; a 'file changed on disk while you have unsaved edits' warning means Revert or Save must resolve it.",
  "'Preview target is not ready yet.' means ShadingUi's previewColor callback returned no texture.",
]
docs = ["/rendering/shading"]
+++

## Volumes panel layout

`VolumeAuthoringUi::Draw` (base) shows, top to bottom: Create Volume presets; for a selected Volume the summary line (solver policy, representation mode, field mask), GPU field size, tiles and residency (it calls `RemoveMissing`, `Ensure` and `SyncAuthoredInputs` on the field storage every frame it is drawn); Rendering / Lighting properties and Render Debug modes (Composite, Scattering, Extinction, Emission, Shadow); the Live Solver for Surface2D5D / Local3D; Sources (9 kinds) and Effectors (5 kinds); Evaluation Order with Up, Down and Remove; Terrain Source Painting; gizmo toggle; Remove Selected Volume. `DrawRepresentationPolicy` (M36) appends Representation / LOD, Volume Cache / Bake and Particle / Surface Output, and the optional region map canvas. Its LOD inputs are clamped: Live radius at least 0.01 m, Passive radius above Live, hysteresis 0-45 %, Coarse resolution 8-128 and steps 8-96, Passive resolution 8-64 and steps 4-32.

## Where operations are reachable

| Operation | Over RPC/MCP |
| --- | --- |
| create, remove, add source/effector, reorder, paint terrain source | yes, as registered commands: `command.catalog`, `command.invoke` (MCP `orbit_invoke_command`, `orbit_invoke_command_by_name`) |
| authored render, output and representation-mode properties | yes, generic `property.set` / `object.get` |
| solver run controls, debug view, render debug mode, LOD policy, follow target, cache bake/import/export/detach | no `volume.*` method or MCP tool was found in `EditorRpcService.cpp`, `StudioRenderViewRpc.cpp` or `orbit_editor_mcp_server.py` |
| Shading tab: tree, create, rename, move, trash, select, source read/write, parameters (numeric and texture), preview settings incl. live compile and mesh, recompile, screenshot, status | yes, `shading.*` |
| Shading Revert button | no `shading.*` method found |

Closing a gap follows AGENTS.md (MCP parity): extract the button's operation, register an RPC, add a tool and document it in `docs/ORBIT_MCP.md` in the same change. Texture parameters in the UI are drop-only (drag a texture from Shading Materials); the RPC takes a content-relative path.

## Shading tab panels

The browser creates folders, shaders (from `shading::ShaderTemplateNames()`) and materials (needs a selected shader or material), renames, trashes to `.orbit/Trash` (recoverable) and moves by drag and drop; shader and material rows get a 32 px thumbnail from the `thumbnailFor` callback. The work area has Preview (shape, lighting, background, sun, exposure, spin, animate; drag orbits, wheel zooms), Parameters (a bare shader's edits last for the session; create a material to save them) and the HLSL source editor. The contract, program assembly and RPC reference live in `/rendering/shading`.
