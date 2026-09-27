# Orbit Shading

Status: **Canonical contract for the Shading tab**

The Shading tab is where shaders and shader materials are authored and tested.
It is two panels in Orbit Studio, both driven by the same `ShadingWorkspace`
model as the `shading.*` RPC methods and MCP tools, so everything a person
can do in either panel an agent can do too
([ORBIT_UI_RULES.md](ORBIT_UI_RULES.md) sections 13 and 22, [ORBIT_MCP.md](ORBIT_MCP.md)):

- **"Shading Materials"** (docked Right): the Content tree, organisation
  (New Folder/Shader/Material, rename, move, trash) and *selection only* --
  every shader and shader material row shows a small live sphere-preview
  thumbnail (`MaterialThumbnailCache`), so browsing reads like Blender's or
  Unreal's material list rather than a bare file tree.
- **"Shading"** (docked Center): the selected shader/material's live preview,
  parameters (including `texture2d` ones, settable by dragging a texture
  asset from Shading Materials) and HLSL editor. It never changes what is
  selected; it only edits and visualizes it.

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
- **Preview** on a sphere, plane, cube or a Wavefront `.obj` mesh from Content,
  under a lighting preset with a chosen background, orbiting the camera with the
  mouse.
- **Browse by thumbnail**: every shader and shader material in Shading
  Materials shows a small static sphere-preview render, not just a file name.
  It re-renders when that asset (or, for a material, the shader it resolves
  to) actually changes; a shader that fails to compile still gets a thumbnail
  (the shared error checker), so a broken material reads as broken rather
  than disappearing.

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
| `OrbitBackdrop(float3 dirWS)` | What the preview background shows in that direction (environment, gradient, gray or checker). Refract into this for glass that visibly distorts what is behind it. |
| `OrbitShapeId()` | The preview shape being drawn: 0 Sphere, 1 Plane, 2 Cube, 3 Mesh. |
| `OrbitToLocal/OrbitToWorld(float3)` | Rotate a world position or direction into/out of the preview object's local space (it is only ever yawed about the origin, never translated or scaled). |
| `OrbitSphereExitDistance(p, d)` / `OrbitBoxExitDistance(p, d, h)` | Exact distance a unit ray travels before leaving a unit-radius sphere / a half-extent-`h` cube centred on the origin, given a start point already on its surface. The real volume a refractive shader needs on the Sphere and Cube preview shapes. |
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
// @param albedo    texture2d Content/Textures/RockGround/RockGround_Diffuse.jpg
```

Types: `float`, `float2`, `float3`, `float4`, `color3` (default values,
optionally range-limited) and `texture2d` (a single content-relative path
token in place of numeric defaults; no range, and `shading.param_set` takes a
path *string* for it rather than a number). Numeric parameters pack in
declaration order into the push-constant budget: **8 scalar values** in
total (an engine limit, reported with a clear message rather than silently
truncated). `texture2d` parameters pack separately into **2 texture slots**
(same reporting). Problems (bad type, duplicate name, over either budget) are
reported with the line number and stop the compile before it starts.

#### Textures

`Sample_<name>(uv)` (pixel stage only, implicit-derivative filtering) and
`SampleLevel_<name>(uv, lod)` (any stage) read a `texture2d` parameter.
Content recognises `.png`/`.jpg`/`.jpeg`/`.bmp` (decoded through Windows
Imaging Component, the same decoder `PbrSetImport` uses for material sets) —
drop one under `Content/` and point a `texture2d` default or
`shading.param_set` at its project-relative path. A save to that file (from
any tool) reloads it live, keeping the last good upload if the new one fails
to decode, the same contract as the mesh preview.

**Real vertex displacement.** A `texture2d` parameter named exactly `height`
together with a `float` parameter named exactly `displacement` is a reserved
pair the engine recognises (`ShaderParameterLayout::HasDisplacement`): the
preview compiles a second, displacement-aware vertex stage that samples
`height` and moves the actual vertex position along its normal by
`(height - 0.5) * displacement`, world units. This is genuine geometry
displacement, not a pixel-shader parallax fake — it changes the silhouette,
and it needs enough vertices to show: the Plane shape is a 96x96 grid (not a
single quad) for exactly this reason, and Sphere/Cube/Mesh all displace too,
using their own vertices. Nothing else about a shader using this pair is
special; `height` can still be sampled for ordinary pixel-shading (a detail
normal from its own gradient, for instance) alongside its displacement role.

The project ships `Content/Shading/Ground/RockGround.shade.hlsl` (and a
`RockGround` shader material of it) as the worked example: `albedo` +
`height` textures imported from a CC0 Poly Haven photo-scan, real
displacement on the Plane shape, and a tangent-space detail normal from
`height`'s own gradient for the fine surface detail the 96x96 grid can't
carry by itself.

### Diagnostics

The compiler sees your file after `#line 1 "<file>"`, so messages name **your
file and your line numbers**, for example
`Lunar.shade.hlsl:56:33: error: use of undeclared identifier 'x'`.

### Templates

`lit` (Lambert + GGX, three parameters), `unlit`, `lunar_regolith`
(Lommel-Seeliger with an opposition surge and procedural craters; try Space
lighting on a sphere), `debug_normals`, `glass_low`, `glass_medium`,
`glass_high`. The preview is opaque, so glass sees the backdrop
(environment/gradient/gray/checker), not other objects.

#### Glass: three cost tiers, not one shader

There is no single "glass" template, on purpose: a shader that ray-traces an
exact refraction path, disperses it into three colour channels and blurs both
the reflection and the transmission for frost costs a lot more per pixel than
one that doesn't, and a scene with more than one or two pieces of glass in it
needs to be able to choose. Pick per-material, not by editing a shader down:

| Template | Worst-case `OrbitBackdrop` samples/pixel | What it does |
| --- | --- | --- |
| `glass_low` | 2 (a little more with `frost`) | No shape query at all: one reflection sample, one naive front-face-only refraction sample, flat authored `thickness` for absorption. Works identically (and identically cheaply) on every shape, including Plane and Mesh. For background glass. |
| `glass_medium` | 6 | Exact ray-traced entry-to-exit path on Sphere/Cube (correct edge bending, correct per-pixel absorption thickness), single refraction (no dispersion), 3-tap frost blur. The right default for most glass in a scene. |
| `glass_high` | 24 | `glass_medium`'s exact path traced three times for per-channel dispersion, plus a 6-tap frost blur. For a hero object or a close-up gem, not a scene full of glass. |

Sphere and Cube are analytic primitives centred on the origin, so
`glass_medium`/`glass_high` can query their exact volume with
`OrbitShapeId`, `OrbitToLocal`/`OrbitToWorld` and
`OrbitSphereExitDistance`/`OrbitBoxExitDistance`. Plane and Mesh have no such
geometry (no back-face to ray-trace against, and no depth-peel pass exists to
provide one), so every tier falls back to treating them as a thin
parallel-faced slab there: parallel faces barely deflect the transmitted ray
(correctly — a window pane doesn't either) and only tint it by the authored
`thickness`. All three take `tint`, `ior`, `absorption`; `glass_medium`/`glass_high`
add `frost`, and `glass_high` alone adds `dispersion`. `thickness` exists on
all three but only does anything on Plane/Mesh (or on every shape, for
`glass_low`, which never queries the real geometry).

The project ships ClearGlass/FrostedGlass (`glass_medium`), AmberGlass
(`glass_low`) and Diamond (`glass_high`) — render them on the Cube shape to
see each tier's edge behaviour clearly.

### Shapes, lighting, backgrounds

| Shapes | Lighting presets | Backgrounds |
| --- | --- | --- |
| `sphere`, `plane`, `cube`, `mesh` | `studio` (key, fill, rim), `sun` (sun and sky), `overcast`, `sunset`, `space` (one hard sun, no ambient) | `environment`, `gradient`, `gray`, `checker` |

The sun direction (azimuth, elevation), exposure and object spin are adjustable.

### Mesh preview

Click a `.obj` in the tree (or `shading.preview_set {"mesh": "<path>"}`) to use
it as the preview mesh; the shape switches to `mesh` and the open shader is left
alone. Meshes are a preview subject, not an edited asset.

- **Parsing:** `v`, `vt`, `vn` and `f` (positive or negative indices; polygons
  fan-triangulate, so they must be convex). `o`, `g`, `s`, `usemtl` and
  `mtllib` are ignored: the shader under test provides the material.
- **Fitting:** the model is centred and scaled to fit a unit sphere, so any model
  lands at a sensible size beside the built-in shapes. Its original radius is in
  `shading.status.mesh.source_radius`.
- **Missing data:** without `vn`, smooth area-weighted normals are generated per
  position (no seams at UV splits); without `vt`, UVs are a spherical projection.
  `had_normals` / `had_uvs` report which.
- **Depth:** the preview has a depth buffer, so non-convex models (a knot, a
  character) draw correctly.
- **Hot reload:** a mesh saved by another tool reloads in the running Studio
  (about 0.2 s). A save that does not parse keeps the last good mesh on screen
  and reports `line N: ...` in `mesh.error`. Rename and move follow the mesh;
  trashing it returns the preview to a sphere.
- **Buffers:** a new mesh is a new pair of GPU buffers; the old pair is retired
  after 8 frames like a replaced pipeline, never destroyed under in-flight
  frames.
- **Other formats:** `.fbx`, `.gltf` and `.glb` are indexed as Mesh assets but
  cannot be previewed yet; the request is refused with a clear message.

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
| `orbit_shading_select(path)` | `shading.select` | Open a shader or material; a `.obj` becomes the preview mesh. |
| `orbit_shading_source_read(path)` | `shading.source_read` | Read source. |
| `orbit_shading_source_write(path, text)` | `shading.source_write` | Write, open, compile live. |
| `orbit_shading_status` | `shading.status` | Selection, compile state, diagnostics, parameters, preview. |
| `orbit_shading_param_set(name, value)` | `shading.param_set` | Set a parameter (saved into an open material): a number or array for float/color3, a content-relative path string for `texture2d`. |
| `orbit_shading_param_reset(name)` | `shading.param_reset` | Back to the declared default. |
| `orbit_shading_preview_get` / `_set` | `shading.preview_get` / `_set` | Shape, mesh (`.obj` path), lighting, background, sun, exposure, spin, animate, camera. |
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

- **Meshes:** only Wavefront `.obj` previews; `.fbx` / `.gltf` are not loaded,
  and `.obj` materials (`mtllib`) and multiple objects are not distinguished.
- **Celestial backdrop:** previewing on a real scene from body coordinates is
  not implemented; backgrounds are procedural.
- **Cooking:** shading shaders are not cooked into builds yet; they are authoring
  assets. `build.cook` ignores the new kinds.
- **Parameter budget:** 8 scalar values per shader (push constants); lifting it
  needs a per-frame buffer ring.
- **Compile thread:** compiles run on the UI thread (typically 15 to 100 ms);
  moving them to the job system is the next improvement.
- **Textures:** 2 `texture2d` slots per shader, no mipmaps beyond what WIC's
  decode gives (a single full-resolution level), no texture arrays, and the
  vertex-stage `height` role is a single reserved name (one displacement
  source per shader, not a stack of them).

## Verification

- `OrbitShadingTests` (needs DXC): every template compiles against the contract,
  diagnostics name the user's file and line, the parameter parser and budget,
  workspace edit / external-change / dirty-buffer / live-compile / material /
  rename / move / trash behaviour, the RPC surface and its error codes, and
  screenshot conversion.
- The mesh tests (in `OrbitShadingTests`): OBJ parsing (quads, negative indices,
  `v//vn`, generated normals and UVs, fitting), line-numbered errors, choosing a
  mesh without disturbing the open shader, external reload, keep-last-good on a
  bad save, rename/move/trash following, and the RPC surface.
- The texture tests (in `OrbitShadingTests`): `texture2d` parsing and its
  budget, the `height`+`displacement` convention, a real WIC decode of a
  hand-encoded BMP end to end through the workspace (default path, override,
  a missing file's keep-last-good error, reset) and through the RPC layer
  (`shading.param_set` with a path string, rejecting a number for a texture
  parameter, `shading.status`'s `textures` array).
- `OrbitContentTests`: folders, atomic writes, path safety, the new asset kinds
  and dependency on the shader.
- `OrbitHotReloadChangeClassifierTests`: `.shade.hlsl` routes to Shader,
  `.orbitshadermaterial` to Content, the atomic-write temp file is ignored.
