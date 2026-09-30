# Orbit Primitives

Status: **Slice 1 (data model) implemented. Rendering, picking and the viewport gizmo are not built yet.**

A `Primitive` is an ordinary semantic object (`kPrimitiveType`, category *Scene / Geometry*): a box, sphere,
cylinder, capsule or plane placed relative to its parent object's frame. It is the first scene object that is
meant to be *seen*; Visibility Proxies are lighting-only and never drawn.

## Data model

| Property | Type | Default | Notes |
| --- | --- | --- | --- |
| Enabled | bool | true | Disabled primitives are skipped by the resolver, not deleted. |
| Shape | int 0..4 | 0 | 0 Box, 1 Sphere, 2 Cylinder, 3 Capsule, 4 Plane. Range-checked by the schema. |
| Parent-frame Position | Vector3 (m) | 0,0,0 | Relative to the parent object's frame. |
| Parent-frame Euler Rotation | Vector3 (deg) | 0,0,0 | |
| Size | Vector3 (m) | 1,1,1 | Full bounding dimensions in local axes. Sphere = ellipsoid in the box; cylinder and capsule run along local +Y; plane is a thin quad in local XZ (Y ignored). |
| Material Asset | string | empty | Project asset reference; empty means the default surface. |
| Cast Shadows | bool | true | Advanced. |

`world_model::ResolvePrimitives(objects, root?)` (`engine/world_model/.../PrimitiveBinding.hpp`) turns the object
tree into `ResolvedPrimitive` records for the renderer and tools. Disabled primitives and primitives with a
non-finite or non-positive size are skipped, never returned as partial data.

Because it is a normal object type, primitives are already reachable everywhere objects are: `object.create`
(type id from `schema.catalog`), `object.reparent`, `object.duplicate`, `object.delete`, `property.set`, the
Explorer, the Properties panel and element bubbles.

## Planned slices

1. **Data model** (done): schema, resolver, test.
2. **Rendering**: a mesh pass in the Studio viewport drawing resolved primitives, lit and shadowed, with the
   assigned material.
3. **Picking and gizmo**: click-select in the viewport and Move/Rotate/Scale drags that write the transform
   properties (one undo step per drag). The gizmo state (`GizmoSettings`) exists; the viewport drag code does not.
4. **Toolbar**: switch the Scene toolbar's `+ Box` / `+ Sphere` from Visibility Proxies to Primitives and add
   Cylinder, Capsule and Plane.

## Hot iteration

- **Saving the resolver/schema source while Orbit runs:** falls under the central watcher's automatic
  native/generation fallback (unknown engine sources), not a bespoke path.
- **Editing a primitive's properties:** direct resource refresh; nothing is rebuilt.
- **Failed patch:** the running generation stays alive (unchanged watcher behaviour).
- **Renderer slice (2):** must keep GPU resources behind deferred retirement and put mesh generation behind a
  reloadable module boundary rather than in the immutable host.
