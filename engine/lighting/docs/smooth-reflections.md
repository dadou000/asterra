+++
path = "/rendering/lighting/smooth-reflections"
title = "Smooth triangle reflections and shared glass environment"
kind = "subsystem"
status = "experimental"
owner_module = "OrbitLighting"
summary = "Selective native-resolution smooth reflections trace one cached opaque triangle scene through software BVH or hardware-assisted exact intersections, shade the hit with material factors and surface-field lighting, and reconstruct reflected radiance independently of scene TAA. Analytic glass uses the same software scene."
keywords = ["mirror", "glass", "reflection", "triangle", "bvh", "roughness", "temporal", "hit distance", "neon noir", "software ray tracing"]
sources = [
  "engine/lighting/include/orbit/lighting/ReflectionScene.hpp",
  "engine/lighting/include/orbit/lighting/ReflectionTraceShader.hpp",
  "engine/lighting/include/orbit/lighting/SdfTraceShader.hpp",
  "engine/lighting/src/HybridReflectionRenderer.cpp",
  "engine/mesh_render/src/ReflectionBvh.cpp",
  "engine/mesh_render/src/MeshSdfScene.cpp",
  "engine/mesh_render/src/MeshLibrary.cpp",
  "engine/mesh_render/src/GlassSurface.cpp",
  "engine/studio_ui/src/StudioViewportLightingPass.cpp",
  "engine/studio_ui/src/StudioViewportCompose.cpp",
  "engine/studio_ui/src/StudioRenderViewRpc.cpp",
  "engine/studio_ui/src/StudioShellInspector.cpp",
  "tools/mcp_server/orbit_editor_mcp_server.py",
]
symbols = ["HybridReflectionRenderer", "ReflectionSceneInput", "BuildReflectionBvh", "MeshSdfScene", "GlassSurfaceRenderer"]
invariants = [
  "MeshLibrary builds local opaque triangles on its existing loader thread; MeshSdfScene updates transformed scene geometry when its model-generation/pose signature changes or its camera-relative anchor drifts 1.5 km. No second content loader or lighting cache is introduced.",
  "Software traversal is stackless preorder (four-triangle leaves); hardware ray queries prune procedural AABBs then intersect the same triangles. Exhausting the 1024-visit budget reports unresolved, never a partial nearest hit or a proven miss.",
  "Only receiver roughness at or below reflection_maximum_roughness (default 0.25) performs opaque reflection work. A closer SDF surface can occlude an exact triangle; unresolved triangles retain SSR, SDF and environment/cache fallbacks. SDF coverage remains approximate.",
  "Exact hit attributes are barycentric vertex normals and material factors. Nearby hits reuse facing SDF albedo/indirect light, replacing its approximate direct term with GGX/Lambert sun shading and exact-triangle plus SDF shadow visibility; valid farther field hits reuse cached radiance. Outside the field, factors/emission and sky irradiance are used. SDF reconstruction rejects outside-volume, empty and opposing-face samples.",
  "History contains scene-linear reflected radiance and hit distance, never the Fresnel-weighted or already lit scene. Virtual reflected-image reprojection, point metadata, normal/distance rejection, edge-aware neighbourhood clamping and a capped 0.65 history weight precede one Fresnel composite. Geometry/lighting/lens/frame/body/cut/resize/discontinuous-use changes reject history. Mixed screen/cache edge samples have no single hit distance and skip history. Scene TAA still operates afterwards.",
  "BeginFrame/BeginReflectionFrame run once from Compose after the application waits for the current submission slot. New triangle, history and parameter resources retire only after every potentially referring slot has completed its fence; multiple view draws do not advance retirement.",
  "UI, RPC and MCP share StudioTerrainLayerOptions: reflection_exact_triangles, reflection_temporal, reflection_maximum_roughness (0..1), reflection_distance_meters (0.1..1000) and reflection_debug_view (0 scene, 1 radiance, 2 distance). Glass remains analytic and uses software triangle queries with its existing 60 m environment range.",
  "Implementation and embedded shader edits use the central automatic native Studio-generation handoff. Candidates compile/validate before activation; failed candidates preserve the active generation. History is presentation cache and intentionally restarts across a generation swap.",
]
related = ["/rendering/lighting/visibility-and-reflections", "/authoring/static-mesh", "/authoring/primitives", "/editor/mcp-rpc"]
verify = [
  "ctest -R Orbit.ReflectionBvh (nearest hits, transforms, malformed inputs, randomized exhaustive comparison).",
  "ctest -R Orbit.ReflectionShaders (actual software/hardware/composite/glass runtime sources compile with DXC; no adapter required).",
  "python tools/orbit_docs_cli.py check; python tools/orbit_docs_cli.py coverage --require-modules",
  "On Vulkan hardware: place a mirror beside a thin textured wall and an offscreen emissive primitive; orbit_view_terrain_layers_set(reflection_temporal=false, reflection_debug_view=1), compare exact triangles on/off, then pan with temporal on and verify no trails across edges.",
  "Repeat with glass sphere/box/cylinder/capsule/plane, multiple views, resize, primitive move, mesh hot reload and camera cut. Capture Vulkan validation output and Reflections GPU timestamps on software and hardware lighting plans.",
]
verified = "f842a956"
+++

The video informs selective work, hit shading and independent history; its
vendor claims and timings are not Orbit measurements. Checkerboard/0.7-scale
tracing, glass line interleaving, surface cards/cubemaps, removal of clears/copies,
partial depth passes and Hi-Z/indirect compaction remain benchmark-driven follow-ups.
The first implementation keeps native mirror detail and one clear fallback chain.

Limits: alpha-cutout/blended triangles are omitted; their field fallback is
approximate. There is no hit-UV texture sampling, recursive mirror-to-mirror
bounce, full local-light shading at offscreen hits or glass-through-glass.
The field's direct/indirect split is approximate, especially for textured metals.
Five RGBA16 targets cost 40 bytes/pixel per active view (about 83 MB at 1080p,
332 MB at 4K), before normal scene targets. CPU scene builds and hardware AS
builds are synchronous on geometry edits. Budget exhaustion and field residency
can change the chosen fallback. Live GPU image quality and timing require the
acceptance checks above; shader compilation alone does not establish either.
