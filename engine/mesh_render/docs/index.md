+++
path = "/authoring/static-mesh"
title = "Static Mesh rendering"
kind = "subsystem"
status = "experimental"
summary = "Streams imported meshes to the GPU (worker-thread load, upload through the frame command list, mip chains, hot reload) and rasterises them into the deferred surface buffer as rigid surfaces."
owner_module = "OrbitMeshRender"
keywords = ["static mesh", "gltf", "mesh surface", "mesh library", "mesh.status"]
sources = [
  "engine/mesh_render/include/orbit/mesh_render/MeshLibrary.hpp",
  "engine/mesh_render/include/orbit/mesh_render/MeshSurface.hpp",
  "engine/mesh_render/CMakeLists.txt",
]
symbols = ["MeshLibrary", "MeshSurfaceRenderer"]
invariants = [
  "Loading only advances inside the MeshSurfaces pass, so that pass must run whenever a mesh is requested, even before any model is resident.",
  "Replaced or released models and staging buffers are retired after 24 pumps, never destroyed under in-flight frames.",
  "A failed reload keeps the previous model.",
  "The mesh shadow resolve (MeshShadow.cpp) is quad based like the proxy shadow: sky openness (16 sky directions) is evaluated once per 2x2 quad and rebuilt per pixel with spatial x normal x depth weights; PCSS sun visibility is reused only where the 3x3 neighbourhood is fully lit or fully shadowed on a planar surface.",
  "The sky atlas and the sun shadow map are re-rendered only when their input signature (instance models and camera-relative rows, window centre/radius/basis, sky up, sun direction, and a 64-frame epoch for streaming textures) changes; the passes are not recorded otherwise, and the persistent targets keep their contents.",
  "MeshSdfScene::Light stops dispatching once its inputs (sun, sky, up, volume revision) have been unchanged for 48 calls; any change restarts the full refresh cycle.",
  "The mesh sun shadow resolve takes a temporalIndex (the TAA frame counter while TAA is on, else 0) that shifts the pixel seed of the PCSS and sky-openness sampling every frame, so TAA averages the penumbra noise; with a fixed seed the pattern is identical every frame and shows up as frozen dithering along shadow edges.",
  "MeshLibrary::AcquireGenerated(key, build) loads a model from a generator instead of a file (the generator runs once on the loader thread, the key must encode all of its inputs, there is no hot-reload stat check, and the generator is dropped when the idle entry is released). See /authoring/primitives for the primitive and glass renderers.",
]
related = ["/authoring/mesh-import"]
verify = ["ctest -R Orbit.MeshRender"]
verified = "1229ef74"
+++

Diagnose: `mesh.status` shows loading / ready / failed with the error text; meshes only resolve under the viewport target body.

See `docs/ORBIT_STATIC_MESH.md` for the pipeline, controls and known limits (no mesh-cast sun shadows yet).
