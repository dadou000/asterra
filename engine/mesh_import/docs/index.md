+++
path = "/authoring/mesh-import"
title = "glTF / GLB mesh import"
kind = "subsystem"
status = "experimental"
summary = "CPU-only glTF 2.0 importer: flattens the node graph into one vertex/index set grouped by material, generating missing normals and tangents, and keeps encoded images for the renderer to decode."
owner_module = "OrbitMeshImport"
keywords = ["gltf", "glb", "mesh import", "static mesh", "sponza"]
sources = [
  "engine/mesh_import/include/orbit/mesh_import/GltfImporter.hpp",
  "engine/mesh_import/include/orbit/mesh_import/MeshAsset.hpp",
  "engine/mesh_import/CMakeLists.txt",
]
symbols = ["ImportGltfFile", "MeshAsset"]
invariants = [
  "Recoverable problems become MeshAsset::warnings; only unusable files throw MeshImportError.",
  "Draco/meshopt geometry is refused, never partially imported.",
  "Mirrored node transforms flip triangle winding and tangent sign.",
]
related = ["/authoring/content", "/authoring/static-mesh"]
verify = ["ctest -R Orbit.MeshImport (set ORBIT_TEST_GLB to also import a real file)"]
verified = "8523e82c"
+++

Imports `.glb` and `.gltf` into a `MeshAsset` in glTF axes and metres. See `docs/ORBIT_STATIC_MESH.md`.
