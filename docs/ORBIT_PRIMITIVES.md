# Orbit Primitives

Status: **Data model, Studio creation, rendering (standard, mirror, emissive and glass surfaces with sun caustics) and the
Move/Rotate/Scale gizmo are implemented. Click-picking in the viewport is not built yet.**

A `Primitive` is an ordinary semantic object (`kPrimitiveType`, category *Scene / Geometry*): a box, sphere,
cylinder, capsule or plane placed relative to its parent object's frame. Unlike Visibility Proxies (lighting only,
never drawn) it is a visible scene object. Like Static Meshes it draws while the viewport targets the body it sits under.

## Data model

| Property | Type | Default | Notes |
| --- | --- | --- | --- |
| Enabled | bool | true | Disabled primitives are skipped by the resolver, not deleted. |
| Shape | int 0..4 | 0 | 0 Box, 1 Sphere, 2 Cylinder, 3 Capsule, 4 Plane. Range-checked by the schema. |
| Parent-frame Position | Vector3 (m) | 0,0,0 | Relative to the parent object's frame. |
| Parent-frame Euler Rotation | Vector3 (deg) | 0,0,0 | |
| Size | Vector3 (m) | 1,1,1 | Full bounding dimensions in local axes. Sphere = ellipsoid in the box; cylinder and capsule run along local +Y (a capsule's radius is half the smaller of X and Z); plane is a thin quad in local XZ (Y ignored; as glass it is a 2 cm pane). |
| Surface | int 0..3 | 0 | 0 Standard, 1 Mirror, 2 Glass, 3 Emissive. |
| Color | Vector3 | 0.8,0.8,0.8 | Standard: albedo. Mirror: reflectance tint. Glass: transmittance per metre of path (white = clear; saturated colours absorb the other channels). Emissive: the emitted hue. |
| Roughness | float 0..1 | 0.5 | Standard only. Mirror is always the smoothest the surface shader allows; glass is always polished. |
| Metallic | float 0..1 | 0 | Standard only (Mirror is fully metallic). |
| Index of Refraction | float 1..3 | 1.5 | Glass only (water 1.33, glass 1.5, diamond 2.4). |
| Emission | float (nits) | 100000 | Emissive only: luminance in cd/m^2 (a sunlit white wall is about 100,000; a phone screen 500; a neon tube 10,000). |
| Caustics | bool | true | Advanced. Glass only: focus sunlight through the body onto receivers. |
| Material Asset | string | empty | Project asset reference; not used by the renderer yet. |
| Cast Shadows | bool | true | Advanced. Glass casts a full sun shadow (the light it transmits returns as caustics). |

`world_model::ResolvePrimitives(objects, root?)` (`engine/world_model/.../PrimitiveBinding.hpp`) turns the object
tree into `ResolvedPrimitive` records for the renderer and tools. Disabled primitives and primitives with a
non-finite or non-positive size are skipped, never returned as partial data. Colour, roughness, metallic and IOR are
clamped to their valid ranges.

## Creating primitives

All three entry points run `world_model::CreatePrimitive`: one undo step, a preset per surface
(`MakePrimitivePreset`: a Mirror is white and fully metallic, a Glass is clear with IOR 1.5, an Emissive is a warm white 100,000 nit emitter).

- **Studio**: *Create > Meshes* has *Primitive Box*, *Primitive Sphere*, *Mirror Sphere*, *Glass Sphere* and *Emissive Sphere*, placed five
  metres in front of the viewport camera and selected.
- **RPC** `primitive.create`: `position` (m, parent frame) is required; `shape` (box / sphere / cylinder / capsule /
  plane), `surface` (standard / mirror / glass / emissive), `parent`, `name`, `euler_degrees`, `size` ([x,y,z] or one number),
  `color`, `roughness`, `metallic`, `ior`, `caustics`, `emission_nits` are optional. `parent` defaults to the primary viewport's target body.
- **MCP** `orbit_primitive_create` (see [ORBIT_MCP.md](ORBIT_MCP.md)).

Because it is a normal object type, primitives are also reachable everywhere objects are: `object.create`
(type id from `schema.catalog`), `object.reparent`, `object.duplicate`, `object.delete`, `property.set`, the
Explorer, the Properties panel and element bubbles.

## Rendering

- **Standard and Mirror** surfaces are generated meshes (`mesh_import::BuildPrimitiveMesh`, sized exactly, so non-uniform
  scales shade correctly) drawn through the static-mesh path: deferred surface buffer, distance-field GI, sun shadows and
  reflections. A mirror is metallic with the minimum roughness; it reflects what the hybrid/exact reflection passes see.
- **Glass** is ray-traced analytically after lighting and the atmosphere (`mesh_render::GlassSurfaceRenderer`): the camera
  ray refracts into the shape, bounces with total internal reflection where the angle demands it, absorbs light by the
  tint, refracts out and looks up what it sees in screen space (so it shows the lit scene, the sky and other objects, with
  an inverted image through a sphere). The Fresnel reflection of the surroundings is added on top. Edges are not
  anti-aliased by the glass pass itself.
- **Emissive** surfaces glow in the surface buffer and light their surroundings (`mesh_render::EmissiveLightRenderer`):
  every pixel receives the analytic irradiance of each emitter treated as a sphere of the same surface area, shadowed by a
  soft ray through the mesh distance field. The light is direct only (an emitter does not bounce its light), up to 16
  emitters are used (the strongest at the camera) and nothing speckles, because GI rays are not asked to find small bright
  sources. In a dark scene the eye adaptation protects against the emitter itself, so the surroundings look dimmer than
  their physical brightness: raise `emission_nits`, or enlarge the emitter, for a stronger glow.
- **Caustics**: a grid of sun photons is traced through the glass, marched to the first surface they hit in the depth
  buffer and splatted as light. The glass casts a sun shadow, so a lit floor shows a dark shadow with a bright focused
  patch inside it, and the patch stretches along the ground at low sun. Photons whose emission point is in shadow (roof,
  other meshes) carry no light, and nothing is added with the sun below the horizon.

Limits: emissive lighting has no bounce and treats long emitters as one sphere; caustics only land on surfaces visible on screen; terrain/proxy shadows are not tested for the glass's sunlight;
frosted glass (roughness), thick-medium scattering and glass seen through glass are not modelled; rays leaving the screen
see a dim sky estimate rather than the real surroundings.

## Hot iteration

- **Saving the resolver/schema/renderer source while Orbit runs:** falls under the central watcher's automatic
  native/generation fallback (unknown engine sources), not a bespoke path.
- **Editing a primitive's properties:** direct resource refresh; nothing is rebuilt. A size or material change selects
  another generated mesh (the old one is released after the idle time).
- **Failed patch:** the running generation stays alive (unchanged watcher behaviour).
- GPU resources: the glass backdrop copy is per view and recreated on resize; the per-frame record buffers retire after 24 draws.

## Remaining

1. Click-select in the viewport (Move/Rotate/Scale handles already work, driven by the Scene toolbar).
2. Frosted glass (roughness), coloured shadows and glass in the distance field.
3. A Material Asset binding so primitives can use authored materials.
