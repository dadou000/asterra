+++
path = "/authoring/mesh-sdf-gi"
title = "Mesh distance fields and SDF GI"
kind = "subsystem"
status = "experimental"
summary = "Builds an exact unsigned distance field per imported mesh; mesh_render merges them into a global field with a surface-voxel radiance cache that the final gather traces when screen traces cannot answer (off-screen bounce, e.g. sunlit ground onto ceilings)."
owner_module = "OrbitMeshSdf"
keywords = ["sdf", "distance field", "gi", "bounce", "final gather", "gi_intensity", "bypass_sdf_gi"]
sources = [
  "engine/mesh_sdf/include/orbit/mesh_sdf/MeshSdf.hpp",
  "engine/lighting/include/orbit/lighting/SdfTraceShader.hpp",
  "engine/mesh_render/src/MeshSdfScene.cpp",
  "engine/mesh_sdf/CMakeLists.txt",
]
symbols = ["BuildMeshSdf", "MeshSdfScene", "kSdfTraceHlsl"]
invariants = [
  "The per-mesh field is built on the loader worker thread; the render thread only merges and lights it.",
  "The final gather falls back to the field only for rays the screen trace did not resolve; bypass_sdf_gi disables it for A/B checks.",
  "Visibility Proxies (analytic boxes/spheres) and a 193x193 1 m terrain height patch around the first placed mesh are stamped into the field by MeshSdfScene::Update (SdfExtraGeometry); meshes win over them on surface voxels. The patch is resampled on the CPU only when the anchor moves 2 m or the terrain source revision changes. bypass_sdf_terrain / bypass_sdf_proxies leave them out for A/B.",
  "The volume gets a 16 m ring around the meshes when extras are present (2 m otherwise); terrain and proxies outside it are not represented.",
  "gi_intensity defaults to 1.0; larger values (e.g. pi) over-expose the whole GI and must be tuned with exposure in mind.",
  "MeshSdfScene mirrors the finished merged distance volume as one uint4 of eight f16 trilinear corners per cell (SdfSceneVolume::distanceCorners), rebuilt after every merge/primitive/terrain stamp; only the final gather's trace reads it, the lighting and debug passes keep the f32 volume.",
]
related = ["/authoring/static-mesh"]
verify = ["ctest -R Orbit.MeshSdf"]
verified = "1229ef74"
+++

Diagnose: set `sdf_debug_view` (1-5) via `view.terrain_layers_set` to inspect the field and stored radiance; compare `bypass_sdf_gi` true/false at the same pose.
