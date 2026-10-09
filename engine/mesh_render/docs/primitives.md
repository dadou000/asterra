+++
path = "/authoring/primitives"
title = "Primitives: mirror, glass with caustics, emissive"
kind = "subsystem"
status = "experimental"
owner_module = "OrbitMeshRender"
summary = "Primitives (box, sphere, cylinder, capsule, plane) draw as generated meshes. Surface Mirror is a perfect reflector, Glass is ray-traced analytically with refraction and sun caustics, Emissive glows and lights its surroundings analytically."
keywords = ["primitive", "emissive", "emission", "lamp", "area light", "glass", "mirror", "caustics", "refraction", "fresnel", "photon splatting", "primitive.create", "orbit_primitive_create"]
sources = [
  "engine/mesh_render/include/orbit/mesh_render/GlassSurface.hpp",
  "engine/mesh_render/src/GlassSurface.cpp",
  "engine/mesh_render/include/orbit/mesh_render/EmissiveLights.hpp",
  "engine/mesh_render/src/EmissiveLights.cpp",
  "engine/mesh_render/tests/EmissiveLightsTests.cpp",
  "engine/mesh_import/include/orbit/mesh_import/PrimitiveMesh.hpp",
  "engine/mesh_import/src/PrimitiveMesh.cpp",
  "engine/studio_ui/src/StudioViewportPrimitives.cpp",
  "engine/world_model/include/orbit/world_model/PrimitiveBinding.hpp",
  "engine/mesh_render/tests/GlassSurfaceTests.cpp",
  "docs/ORBIT_PRIMITIVES.md",
]
symbols = ["EmissiveLightRenderer", "EmissiveLight", "SelectEmissiveLights", "GlassSurfaceRenderer", "GlassInstance", "GlassLighting", "BuildPrimitiveMesh", "MakePrimitiveModelRequest", "CreatePrimitive", "PrimitiveSurface"]
invariants = [
  "Primitive geometry is generated at the authored size (instance scale is 1; sizes are baked into vertices) and acquired through MeshLibrary::AcquireGenerated under a key that encodes shape, size and, for opaque surfaces, the material; a property edit just selects another key and the old model is released after the idle time. Generated models are never stat-checked for hot reload.",
  "Standard and Mirror primitives are ordinary opaque mesh instances (deferred surface buffer, distance field, sun shadow, reflections). A Mirror is metallic 1 at the smoothest roughness the surface shader allows (clamped to 0.045).",
  "Glass is never in the surface buffer or the distance field. It is evaluated analytically (ellipsoid, box/plane slab, elliptic cylinder, capsule as a union) in three render-graph passes after ComposeAtmospherePasses: GlassCaustics (additive photons), GlassBackdropCopy (copy of the lit scene), Glass (replaces the colour under the body). It writes only scene colour, never depth.",
  "A glass ray enters with Fresnel reflection, refracts, follows up to four segments inside (total internal reflection bounces, Beer-Lambert absorption pow(tint, metres)), exits with the second Fresnel term and is traced in screen space against the depth buffer; rays that leave the screen see the sky pixel they point at, else GlassLighting::environment (a fifth of the sky radiance).",
  "A Plane in glass is a pane of kGlassPaneHalfThicknessMeters (1 cm) half thickness. The instance transform must stay rotation + translation (the shader inverts it by transpose).",
  "Caustics: one photon per cell of a G x G grid over the body's bounding disc (G = GlassPhotonGrid, 96..224), traced through the glass, marched to the first receiver in the depth buffer and splatted as a Gaussian whose width follows the spacing of neighbouring photons on the receiver plane (clamped to 0.7 x emission spacing .. 0.6 m). Photon power = sun irradiance x cell area x transmitted fraction; the splat adds albedo/pi x power x cos x kernel, so the energy of the beam shadowed by the glass returns as light.",
  "Glass casts the sun shadow (GlassInstance::castShadows) so the shadow plus caustic is energy-consistent; photons whose emission point is shadowed in the mesh sun shadow map carry no light. The map covers meshes and glass only: terrain/proxy occluders are not seen, and the sun is also cut off smoothly at the local horizon (GlassLighting::sunVisibility).",
  "Emissive limits: only sphere-equivalent emitters (a long capsule lights as one sphere at its centre), no emitter-to-emitter or bounce light, up to 16 emitters, and Mirror/Glass/other opaque surfaces see the emission only through the lit scene.",
  "Glass roughness, frosted glass, multiple internal reflections beyond total internal reflection, caustics on receivers outside the screen and glass seen through other glass are not modelled.",
  "Emissive primitives are opaque mesh instances whose material has emissiveFactor = color x emission_nits / kSceneLuminanceNitsPerUnit (929,563 nits per scene unit) and a near-black base colour, so the surface itself glows in the surface buffer. They set MeshMaterial::emissiveInGi = false: the distance field keeps no emissive voxels for them, because small bright voxels found by chance by GI rays are what makes emitters speckle.",
  "EmissiveLightRenderer (passes EmissiveLights compute + EmissiveLightsComposite, before the atmosphere, skipped by bypass_indirect_lighting / bypass_mesh_surfaces) lights every pixel from up to 16 emitters (SelectEmissiveLights keeps the strongest at the camera): Lambertian sphere-light irradiance E = pi L sin^2(alpha) cos(theta) with a wrapped cosine, using the sphere of equal surface area (EmissiveSurfaceArea / EquivalentSphereRadius), and a SdfSoftShadow ray that stops short of the emitter's bounding sphere so it does not shadow itself. With no distance field the lights simply cast no shadows. Light only flows direct from emitter to receiver: emitters do not bounce light.",
  "The final gather's screen-space hits subtract the emission of pixels whose emission exceeds 0.02 scene units (about 18,600 nits): strong emitters are lit analytically and would otherwise add speckle when rays find their bright pixels.",
  "primitive.create (RPC) / orbit_primitive_create (MCP) / the Create > Meshes entries all call world_model::CreatePrimitive: one undo step, a preset per surface (MakePrimitivePreset).",
]
related = ["/authoring/static-mesh", "/authoring/mesh-import", "/rendering/lighting"]
verify = ["ctest -R \"Orbit.(PrimitiveMesh|PrimitiveBinding|GlassSurface|EmissiveLights)\"", "primitive.create an emissive sphere (emission_nits 300000) in the shaded nave: the nearby floor and curtains pick up its colour with soft shadows and no added speckle (compare with the primitive disabled)", "Launch a scratch project, primitive.create a glass sphere at noon (time.set local_time_seconds 43200) over a sunlit floor: a bright caustic spot inside the sphere's shadow, gone with caustics false"]
verified = "1229ef74"
+++

Hot iteration: the shaders live in `GlassSurface.cpp` and primitive generation in `PrimitiveMesh.cpp`; both fall under the central watcher's native/generation fallback. Editing any primitive property is a direct resource refresh (the generated model is re-keyed, the glass passes read the new values next frame).

Diagnose: `mesh.status` lists generated models as `generated/primitive/...`. A glass primitive that does not draw needs the viewport to target its parent body and `bypass_mesh_surfaces` off. No caustic means: sun below the horizon, the sun shadowed at the glass (see the mesh sun shadow), `caustics` off, or no receiver on screen within 80 m of the glass along the refracted beam. A speckled caustic means too few photons for a body that large (raise `kMaxPhotonGrid`).
