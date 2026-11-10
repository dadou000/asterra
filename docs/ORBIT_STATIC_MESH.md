# Orbit Static Mesh (glTF / GLB import)

Status: **Implemented**: import (`mesh.import`), scene object, lit and textured rendering, sun shadows, Move / Rotate /
Uniform-scale gizmo and RPC. Not yet built: click-picking in the viewport, alpha-blended (sorted) materials.

A `Static Mesh` object (`kStaticMeshType`, category *Scene / Geometry*) places an imported glTF 2.0 file relative to its
parent object's frame. Properties: Enabled, Mesh Asset (project-relative path, e.g. `Content/Models/Sponza/scene.glb`),
Parent-frame Position (m), Euler Rotation (deg, Rz*Ry*Rx), Uniform Scale, Cast Shadows (reserved).

## Pipeline

| Stage | Where |
| --- | --- |
| Parse `.glb` / `.gltf` (sparse accessors, quantized attributes, node transforms baked, tangents and normals generated when missing, geometry grouped per material) | `engine/mesh_import` (CPU only, unit tested) |
| Worker-thread load, PNG/JPEG decode (WIC `DecodeTextureMemory`), GPU upload through the frame's command list, mip chains, hot reload on file change | `engine/mesh_render` `MeshLibrary` |
| Rasterise into the deferred surface buffer after terrain and proxies (class RigidGeometry / LocalMesh) | `MeshSurfaceRenderer`, wired in `StudioViewportRenderer.cpp` |

Textures are uploaded as sRGB (base colour, emissive) or linear (normal, metallic-roughness) with a full mip chain and
repeat addressing (`TextureDesc::mipLevels`, `repeatAddress`, `CommandList::GenerateMipmaps`).

## Observability and control

- RPC `mesh.status` / MCP `orbit_mesh_status`: per mesh state (loading | ready | failed), counts, texture streaming
  progress, bounds and import warnings.
- Layer flag `bypass_mesh_surfaces` (`view.terrain_layers_set`, MCP, Terrain layers checkbox) hides all meshes for A/B.
- `mesh.import` / MCP `orbit_mesh_import` copies a .glb/.gltf (or folder, or .zip via MCP) into `Content/Models/<name>/`
  after validating it; with `parent_id` the MCP tool also creates and places the Static Mesh object.
- Otherwise create and place with `object.create` (type from `schema.catalog`) and `property.set`.
- The Move / Rotate / Scale gizmo and `object.transform` work on Static Meshes (scale is uniform).
- Meshes resolve under the viewport's target body only; the Studio camera must target that body.

## Shadows

Meshes cast sun shadows through a 2048 squared sun-space depth map (`MeshShadowMapRenderer`) that encloses the mesh
instances; `MeshSunShadowRenderer` folds it into the sun-visibility texture direct lighting reads, with
or without the proxy ray-query pass. Terrain, proxies and the meshes themselves all receive it. The window centre is
snapped to texels in a camera-independent frame so edges do not shimmer. `bypass_proxy_sun_shadow` disables it.

Soft shadows use PCSS (contact-hardening): a 16-tap blocker search averages the occluder depth, the receiver-to-occluder
distance times the sun's angular radius (emitter radius over distance, so the real Sun is about 0.27 degrees) sets the
penumbra width, and a 24-tap Vogel-disc filter of that width gives the visibility. Shadows are sharp at contact and
soften with distance (about 0.9 cm of penumbra per metre of occluder distance for the real Sun). `mesh_shadow_softness`
(RPC / MCP / Terrain layers slider) multiplies the sun size: 0 = hard shadows, 1 = physical, larger exaggerates.

Sky fill: mesh pixels carry their own surface tag (class 3, representation `StaticMesh`) so the proxy pass's sky fill,
which only knows proxy boxes, is replaced. Seven 512 squared sky-direction depth maps (one straight up, six at 45 degrees)
live in a 4 x 2 atlas; the resolve pass weights them by the surface cosine, counts the mirrored below-horizon directions as
blocked, and writes sky irradiance times that openness, so covered and enclosed areas go dark like the proxy room does.
There is no bounce light yet, so a lit area does not light its neighbours.

## Hot iteration

- Saving the `.glb`: re-imported on a worker thread (polled every 0.75 s); a failed re-import keeps the current model;
  replaced GPU resources are retired after 24 pumps.
- Editing placement properties: direct refresh, nothing rebuilt.
- Saving engine sources: the central watcher's native/generation path.

## Known limits

No click-picking yet (select the object in the Explorer). Meshes do not receive sky fill or bounce shadows beyond what
the radiance cache gives, vertex colours and secondary UV sets are ignored, `BLEND` materials render as alpha-tested (cutoff 0.5), Draco/meshopt
files are refused with a clear error. glTF files are in metres; scale with Uniform Scale.
