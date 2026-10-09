# Smooth surface reflections

## Behavior
Smooth opaque surfaces and analytic glass trace imported triangles, then fall
back to the existing distance field and environment. Reflection history is
independent of scene TAA and rejects changed hit geometry and lighting.

## Existing owner
- Geometry/content: MeshLibrary and MeshSdfScene (one merged scene).
- Reflection composition: HybridReflectionRenderer and GlassSurfaceRenderer.
- Canonical settings: StudioTerrainLayerOptions, shared by inspector and RPC.

## Primary insertion point
- MeshLibrary::Adopt retains compact local reflection triangles.
- MeshSdfScene::Update maintains a cached triangle BVH alongside its field.
- HybridReflectionRenderer::Resolve selects and shades hits; its history only
  contains reflected radiance, never the already lit scene.
- GlassSurfaceRenderer::EnvironmentTrace consumes the same triangle scene.

## Secondary touch points
Shared lighting shader records, StudioViewportLightingPass, layer RPC/MCP,
inspector, CPU geometry tests and GPU shader compilation tests.

## Must not be implemented in
No second content loader, voxel-lighting cache, watcher, GI model or launcher.
Cubemaps and a surface-card atlas are alternatives, not additional mandatory
passes. Rough diffuse reflections remain in the existing lighting pipeline.

## Validation
BVH nearest-hit, transformed geometry and malformed-input tests; independent
shader compilation; documentation checks; Windows build/CTest when available.
Embedded shaders/native edits follow the existing automatic Studio-generation
handoff. Failed candidates leave the active generation alive; GPU resources
retire only after every potentially referring frame slot has completed its GPU fence.
