# Orbit Shading

Status: **Canonical contract for the Shading tab**

The Shading tab is where shaders and shader materials are authored and tested.
It is one panel in Orbit Studio (a tab beside Viewport), driven by the same
`ShadingWorkspace` model as the `shading.*` RPC methods and MCP tools, so
everything a person can do in the tab an agent can do too
([ORBIT_UI_RULES.md](ORBIT_UI_RULES.md) sections 13 and 22, [ORBIT_MCP.md](ORBIT_MCP.md)).

```text
Shading tab (UI) ─┐
RPC / MCP        ─┼─> ShadingWorkspace ─> ContentService (files under Content/)
external editor  ─┘        │                      ▲
   saves a file            ├─> CompileShadingProgram (DXC)
   (hot-iteration          └─> ShaderPreviewRenderer (Vulkan pipeline swap)
    watcher) ──────────────────────────────────┘
```

## What you can do

- **Organise** the whole Content tree (the Material Service): create folders,
  rename, move (drag and drop in the tab), and trash. Folders are real
  directories, so an external file manager sees the same tree. Trash is
  reversible: entries move to `<project>/.orbit/Trash/<stamp>/`.
- **Write shaders** as HLSL against the shading contract below, in the tab or
  in any editor. Edits recompile live and are swapped into the preview.
- **Bind parameters** with `// @param` lines; the tab builds controls from them
  and a shader material stores overrides.
- **Preview** on a sphere, plane or cube under a lighting preset with a chosen
  background, orbiting the camera with the mouse.

## Assets

| Asset | File | Notes |
| --- | --- | --- |
| Shader | `Name.shade.hlsl` | The `Shade()` function and helpers. Not a standalone HLSL stage: no sidecar, and not cooked as a `Shader`. |
| Shader material | `Name.orbitshadermaterial` | TOML: which shader, plus named parameter overrides. |

```toml
[shader_material]
name = "Gold"
shader = "Lit.shade.hlsl"        # relative to this file's folder

[[shader_material.parameter]]    # declaration order = packing order
name = "tint"
value = [1.0, 0.77, 0.34]

[[shader_material.parameter]]
name = "roughness"
value = 0.25
```

Moving or renaming an asset changes its path-derived id. Paths stored inside
assets (a material's `shader`) are not rewritten.

## The shader contract (v1)

Your file is compiled after an engine prelude. You write `Shade()`; the engine
supplies the vertex stage, geometry, lighting rig and display encoding. Output
is **scene-linear HDR**; the wrapper applies exposure, a filmic tone map and
gamma, so shaders never encode for display.

```hlsl
struct OrbitSurface {
    float3 positionWS;  float3 normalWS;
    float3 tangentWS;   float3 bitangentWS;
    float2 uv;
    float3 viewWS;      // unit vector from the surface toward the camera
    float  time;        // seconds (advances when Animate is on)
};
struct OrbitLight    { float3 directionWS; float3 radiance; };   // direction TOWARD the light
struct OrbitLighting { OrbitLight key, fill, rim; float3 ambientSky, ambientGround; };

float4 Shade(OrbitSurface s, OrbitLighting l);   // you implement this; a = opacity
```

Helpers available to your code:

| Function | Purpose |
| --- | --- |
| `OrbitEnvironment(float3 dirWS)` | Sky radiance for a world direction under the current preset (reflections). |
| `OrbitSunDirection(az, el)` | Unit sun vector from radians. |
| `OrbitHash(float3)`, `OrbitHash3(float3)` | Cheap hash noise. |
| `OrbitParam(i)`, `OrbitParam2/3/4(i)` | Raw parameter slots. |
| `Param_<name>()` | Generated accessor per `// @param` (preferred). |
| `ORBIT_PI` | Constant. |

### Parameters

```hlsl
// @param tint      color3 0.80 0.80 0.80
// @param roughness float  0.55 | 0.03 1        (| min max = slider range)
// @param uvScale   float2 1 1
```

Types: `float`, `float2`, `float3`, `float4`, `color3`. Declaration order is the
packing order. A shader may declare at most **8 scalar values** in total: that is
the push-constant budget of the preview pipeline (an engine limit, reported
with a clear message rather than silently truncated). Problems (bad type,
duplicate name, over budget) are reported with the line number and stop the
compile before it starts.

### Diagnostics

The compiler sees your file after `#line 1 "<file>"`, so messages name **your
file and your line numbers**, for example
`Lunar.shade.hlsl:56:33: error: use of undeclared identifier 'x'`.

### Templates

`lit` (Lambert + GGX, three parameters), `unlit`, `lunar_regolith`
(Lommel-Seeliger with an opposition surge and procedural craters; try Space
lighting on a sphere), `debug_normals`.

### Shapes, lighting, backgrounds

| Shapes | Lighting presets | Backgrounds |
| --- | --- | --- |
| `sphere`, `plane`, `cube` | `studio` (key, fill, rim), `sun` (sun and sky), `overcast`, `sunset`, `space` (one hard sun, no ambient) | `environment`, `gradient`, `gray`, `checker` |

The sun direction (azimuth, elevation), exposure and object spin are adjustable.

## Hot iteration (no restart)

Per [ORBIT_HOT_ITERATION.md](ORBIT_HOT_ITERATION.md): a shader edit is **not** a
Studio rebuild.

| You do | What happens |
| --- | --- |
| Edit in the tab with Live compile on | Debounced (0.35 s) compile of the buffer without saving; preview swaps on success. |
| Press Save and compile, or `shading.source_write` | Atomic write, compile, swap. Diagnostics returned. |
| Save `*.shade.hlsl` in another editor | Central watcher classifies it `Shader`; `ContentService` rescans; the tab reloads and recompiles (about 0.3 s). |
| Save `*.orbitshadermaterial` externally | Classified `Content`; parameter overrides reload. |
| Save with unsaved edits in the tab | The buffer is never clobbered: the tab flags "changed on disk". |
| Save a shader with an error | The preview **keeps the last working program**; diagnostics show. |
| No valid program ever compiled | A magenta checker is drawn, never a blank preview. |

Pipeline swap follows §9 of the hot-iteration contract: the new program becomes
a new pipeline at a frame boundary; the previous pipeline is retired after 8
frames (`ShaderPreviewRenderer::kRetireAfterFrames`) rather than destroyed while
frames may reference it. A failed pipeline creation leaves the working one in
place and is reported to the tab.

Changing the *C++* (the panel, the renderer, the contract prelude) takes the
automatic Studio-generation handoff, not a manual restart. Note: run Studio from
`Orbit.exe` or a staged copy; a process launched directly from
`build/apps/editor/Release/OrbitStudio.exe` locks its own rebuild target and the
fallback build fails at link (`LNK1168`).

## RPC and MCP reference

Errors: `1050` invalid request (bad path/name/parameter, refused operation),
`1051` unexpected failure. Compile errors are **not** errors: they are a normal
result with `compiled: false` and `diagnostics`.

| MCP tool | RPC method | Purpose |
| --- | --- | --- |
| `orbit_shading_options` | `shading.options` | Shapes, lighting, backgrounds, templates, parameter budget. |
| `orbit_shading_tree` | `shading.tree` | Nested Content tree. |
| `orbit_shading_folder_create(path)` | `shading.folder_create` | New folder. |
| `orbit_shading_rename(path, name)` | `shading.rename` | Rename in place. |
| `orbit_shading_move(path, folder)` | `shading.move` | Move into a folder. |
| `orbit_shading_trash(path)` | `shading.trash` | Reversible remove. |
| `orbit_shading_shader_create(folder, name, template)` | `shading.shader_create` | New shader from a template; opens it. |
| `orbit_shading_material_create(folder, name, shader)` | `shading.material_create` | New shader material; opens it. |
| `orbit_shading_select(path)` | `shading.select` | Open a shader or material. |
| `orbit_shading_source_read(path)` | `shading.source_read` | Read source. |
| `orbit_shading_source_write(path, text)` | `shading.source_write` | Write, open, compile live. |
| `orbit_shading_status` | `shading.status` | Selection, compile state, diagnostics, parameters, preview. |
| `orbit_shading_param_set(name, value)` | `shading.param_set` | Set a parameter (saved into an open material). |
| `orbit_shading_param_reset(name)` | `shading.param_reset` | Back to the declared default. |
| `orbit_shading_preview_get` / `_set` | `shading.preview_get` / `_set` | Shape, lighting, background, sun, exposure, spin, animate, camera. |
| `orbit_shading_recompile` | `shading.recompile` | Recompile the buffer now. |
| `orbit_shading_screenshot(path)` | `shading.screenshot` | BMP of the preview. |
| `orbit_panel_list` / `_focus` / `_close` | `studio.panel_list` / `_focus` / `_close` | Open the Shading tab (or any tab) by title. |

Paths are project-relative (`Content/Shading/A.shade.hlsl`) or content-relative
(`Shading/A.shade.hlsl`).

### Worked example

```python
import orbit_editor_mcp_server as m
m.orbit_panel_focus("Shading")
m.orbit_shading_folder_create("Shading/Lunar")
m.orbit_shading_shader_create("Shading/Lunar", "Regolith", "lunar_regolith")
m.orbit_shading_preview_set(shape="sphere", lighting="space",
                            sun_azimuth_degrees=70, sun_elevation_degrees=20)
status = m.orbit_shading_status()             # compiled, diagnostics, parameters
m.orbit_shading_param_set("craters", 0.9)
m.orbit_shading_screenshot("C:/tmp/regolith.bmp")
```

## Limits and next phases

Phase 1 (this document) is the authoring loop. Known limits, by design of the
first slice:

- **Meshes:** an `.obj` preview shape is not implemented (the Mesh asset kind is
  indexed but not loaded).
- **Celestial backdrop:** previewing on a real scene from body coordinates is
  not implemented; backgrounds are procedural.
- **Cooking:** shading shaders are not cooked into builds yet; they are authoring
  assets. `build.cook` ignores the new kinds.
- **Parameter budget:** 8 scalar values per shader (push constants); lifting it
  needs a per-frame buffer ring.
- **Compile thread:** compiles run on the UI thread (typically 15 to 100 ms);
  moving them to the job system is the next improvement.
- **Textures:** shaders cannot yet sample project textures.

## Verification

- `OrbitShadingTests` (needs DXC): every template compiles against the contract,
  diagnostics name the user's file and line, the parameter parser and budget,
  workspace edit / external-change / dirty-buffer / live-compile / material /
  rename / move / trash behaviour, the RPC surface and its error codes, and
  screenshot conversion.
- `OrbitContentTests`: folders, atomic writes, path safety, the new asset kinds
  and dependency on the shader.
- `OrbitHotReloadChangeClassifierTests`: `.shade.hlsl` routes to Shader,
  `.orbitshadermaterial` to Content, the atomic-write temp file is ignored.
