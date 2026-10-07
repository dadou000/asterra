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
]
related = ["/authoring/mesh-import"]
verify = ["ctest -R Orbit.MeshRender"]
verified = "8523e82c"
+++

Diagnose: `mesh.status` shows loading / ready / failed with the error text; meshes only resolve under the viewport target body.

See `docs/ORBIT_STATIC_MESH.md` for the pipeline, controls and known limits (no mesh-cast sun shadows yet).
