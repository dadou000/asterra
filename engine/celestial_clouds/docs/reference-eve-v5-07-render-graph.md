+++
path = "/rendering/clouds/reference-eve-v5-render-graph"
title = "EVE V5 reference Vulkan render graph, resource ABI, scheduling and lifecycle"
kind = "concept"
status = "proposed"
owner_module = "OrbitCelestialClouds"
summary = "Original Orbit code-level design for a third cloud backend: immutable frame snapshot, pass ordering, shader ABI, RHI barriers, async time slicing and editor/debug integration."
keywords = ["Vulkan", "render graph", "shader ABI", "descriptor", "lifetime", "multi view", "profiling", "timewarp"]
related = ["/rendering/clouds/reference-eve-v5", "/rendering/clouds/unified-system-b"]
depends_on = ["/rendering/clouds/reference-eve-v5-medium", "/rendering/clouds/reference-eve-v5-lighting"]
+++

# Non-destructive integration into current Orbit

**EXISTING-ORBIT**:
- `engine/studio_ui/src/StudioViewportAtmospherePass.cpp` composes the atmosphere and then clouds; A has its own temporal history and volumetric light caches; B has independent `.UnifiedB.SunCache`, `.UnifiedB.March`, `.UnifiedB.Composite`.
- `engine/studio_ui/src/StudioViewportLightingPass.cpp` constructs a half-resolution receiver cloud shadow target, consumed by `DirectLightingRenderer::Draw` as the optional cloudShadow texture.
- `engine/studio_ui/src/StudioViewportStorm.cpp` advances/publishes one storm solver export, currently through `sharedStormCloudExport_`, while `CloudPresentation` owns async field generation and a field/GPU product.
- `engine/studio_ui/include/orbit/studio_ui/StudioViewportRenderer.hpp` owns A/B map state and textures keyed by viewport, but several graph lambdas currently capture raw GPU/product pointers.
- `engine/celestial_clouds/include/orbit/celestial_clouds/UnifiedCloudFoundation.hpp` has `FrameSignature`, `Invalidate` and `ResourceBudget`, but a full generation-pinned reference snapshot/allocator is not implemented.

Do not put a third body of cloud shader code into `UnifiedCloudRenderer.cpp` and call it a new renderer. Split clearly into `engine/celestial_clouds/reference/` (new module or isolated target) and a thin Studio adapter. Keep the old A/B shaders unchanged. Single editor interface; third backend is a selector under the existing Cloud renderer control with a disabled/unavailable status until it is valid.

# Exact public C++ boundary (PROPOSED-PORT)

```cpp
namespace orbit::celestial_clouds::reference {

enum class RefReadiness : u8 {
    NoLicensedInputs, MissingMaps, InvalidConfig, Initializing,
    CompilingShaders, AllocatingGpu, WarmingCaches, Ready, Failed
};
struct RefFrameKey {
    u64 worldGeneration;
    universe::BodyId body;
    u64 cloudConfigHash;
    u64 weatherHash;
    u64 stormGeneration;
    u64 terrainRevision;
    u64 atmosphereRevision;
    u64 lightingRevision;
    u64 assetManifestHash;
    u64 backendBuildFingerprint;
    i64 simulationEpoch;
    u32 viewportWidth, viewportHeight;
};
struct RefFrameSnapshot {
    RefFrameKey key;
    celestial_atmosphere::AtmosphereRenderView atmosphere;
    std::shared_ptr<const CloudFieldProduct> weather;
    std::shared_ptr<GpuCloudFieldProduct> gpuWeather;
    std::shared_ptr<const ReferenceCloudMaterials> materials;
    std::shared_ptr<const ReferenceCloudMaps> maps;
    std::shared_ptr<const StormExportTicket> storm; // GPU producer fence
    std::shared_ptr<const TerrainVisibilityTicket> terrain;
    std::shared_ptr<const AtmosphereLutTicket> sky;
    std::shared_ptr<const WeatherEventStream> effects;
    // All sources stay alive through the final consumer fence.
};
struct RefViewResources {
    std::unique_ptr<rhi::Texture> cloudRadianceTransmittance;
    std::unique_ptr<rhi::Texture> cloudFirstDepth;
    std::unique_ptr<rhi::Texture> cloudMotion;
    std::unique_ptr<rhi::Texture> receiverSunT;
    std::unique_ptr<rhi::Texture> temporalHistory[2];
    std::unique_ptr<ReferenceLightVolume> lightVolume;
    u64 residentBytes;
    u64 lastGeneration;
    bool historyValid;
};
class ReferenceCloudRenderer {
public:
    ReferenceCloudRenderer(rhi::Device&, const shader::Compiler&);
    RefReadiness Readiness() const noexcept;
    void ScheduleLightUpdate(render_graph::RenderGraph&,
        const RefFrameSnapshot&, RefViewResources&);
    void ScheduleSurfaceShadow(render_graph::RenderGraph&,
        const RefFrameSnapshot&, RefViewResources&,
        const render_graph::TextureHandle& sceneDepth);
    void ScheduleViewMarch(render_graph::RenderGraph&,
        const RefFrameSnapshot&, RefViewResources&,
        const render_graph::TextureHandle& sceneDepth);
    void ScheduleTemporalResolve(render_graph::RenderGraph&,
        const RefFrameSnapshot&, RefViewResources&);
    void ScheduleComposite(render_graph::RenderGraph&,
        const RefFrameSnapshot&, RefViewResources&,
        const render_graph::TextureHandle& sceneHDR);
};
} // namespace
```

These are proposed signatures; use exact RenderGraph types from `render_graph/RenderGraph.hpp` during implementation. The purpose is a complete API/ownership boundary, not compilation on an unmodified branch.

# Render-graph dependency graph

Required logical dependencies (topological; unrelated passes can overlap once resource ordering is proved):

```text
Physical simulation and cloud/weather/GPU export publication (CPU)
  -> Atmosphere LUT availability / sky irradiance / terrain visibility
  -> Immutable physical per-view Snapshot + pinned GPU source revisions
  -> Ref.CloudDensityLodBuild (compute, only when medium input revisions change)
  -> Ref.SunLightVolumeUpdate (compute, time-sliced)
  -> Ref.ReceiverShadow (graphics/compute, read depth+cloud medium)
        -> existing DirectLighting, direct Sun channel ONLY
  -> existing opaque/HDR scene + correct depth buffer
  -> existing Atmosphere/Aerial integration (or shared per-segment integrator)
  -> Ref.CloudViewMarch (graphics or compute, radiance+T+depth/motion)
  -> Ref.CloudTemporalResolve (independent history; disocclusion)
  -> Ref.CloudComposite (scene linear, once)
  -> Ref.PrecipitationParticles / LightningGeometry / Droplets
  -> common SSR/probes/transparency and final HDR exposure/post
```

A terrain/water/cloud/atmosphere dependency may require two representations of the same physical light for a correct order: surface direct lighting needs cloud shadow earlier than cloud view composition; the light volume can update in compute before either. Never add a second cloud receiver-shadows darkening overlay after DirectLighting. The baseline KSP effect of clouds casting atmospheric god rays must be integrated during atmosphere ray evaluation or via a physically consistent intermediate rather than darkening an already-composited image.

## Annotated graph skeleton

**PROPOSED-PORT**, using Orbit's already observed patterns:

```cpp
void ScheduleReferencePasses(
    render_graph::RenderGraph& graph,
    const std::shared_ptr<const RefFrameSnapshot>& snapshot,
    RefViewResources& resources,
    rhi::Texture& hdrColor,
    rhi::Texture& sceneDepth)
{
    // Pins snapshot and target state through recorded commands;
    // actual implementation additionally retains resources until GPU fence.
    auto depth = graph.ImportTexture(
        "Ref.SceneDepth", sceneDepth, rhi::ResourceState::DepthRead);
    auto medium = graph.ImportTexture(
        "Ref.CloudMedium", *resources.cloudRadianceTransmittance,
        rhi::ResourceState::ShaderResource);
    graph.AddPass(
        "CloudRef.March",
        {
            {.texture=depth,
             .state=rhi::ResourceState::DepthRead,
             .access=render_graph::Access::Read},
            {.texture=medium,
             .state=rhi::ResourceState::RenderTarget,
             .access=render_graph::Access::Write}
        },
        [snapshot, &resources](rhi::CommandList& cmd,
             const render_graph::Resources& graphResources) {
            DrawReferenceCloudMarch(cmd, *snapshot, resources);
        });
    // Add CloudRef.Resolve with prior radiance history, motion and depth,
    // followed by CloudRef.Composite using explicit read/write handles.
}
```

Do not capture a stack-local view object or raw `CloudFieldProduct*` across an async submit. The graph's callbacks are recorded after graph construction; simulation publications must be immutable for their lifetime. Existing B's `sharedStormCloudExport_` is Studio-global and is a known multi-view/multi-body hazard. Introduce a snapshot-scoped `StormExportTicket` that pins the exact buffer and generation for each view.

# Vulkan GPU ABI / pipeline creation

Source is not available in this ZIP, so the V5 Vulkan shader ABI is an **original Orbit ABI**, not a transcription of Unity/DirectX compiled bundles. Make it versioned, explicit and testable.

```cpp
struct alignas(16) RefCloudFrameConstants {
    math::Float4 cameraOriginHigh_bodyRadius; // high/low split elsewhere
    math::Float4 cameraOriginLow_cloudTop;
    math::Float4 forward_tanHalfFov;
    math::Float4 up_invResolutionY;
    math::Float4 sunDir_solarAngularRadius;
    math::Float4 planetRadii;
    math::Float4 sunIrradiance_time;
    math::Float4 raymarchMinMaxScale;
    std::array<u32,4> flagsAndOffsets;
};
static_assert(sizeof(RefCloudFrameConstants)%16==0);
enum class RefDescriptorBinding : u32 {
    WeatherField=0, TypedLayerTable=1, Curves=2, CloudMaps=3,
    NoiseAtlas=4, StormExport=5, CloudLightVolume=6,
    AtmosphereLutSet=7, SceneDepth=8, TerrainSunVisibility=9,
    BlueNoise=10, EffectState=11
};
```

Bind UBO/push constants within RHI device limits, check HLSL↔SPIR-V std430/structured-buffer layout, and keep sampled textures separate from storage resources. Shader stages compiled through Orbit's `shader::Compiler` must expose SPIR-V entrypoint `main` because the current VulkanPipeline path requests that name; we already encountered pipeline failures when B used non-main entrypoints. Validate SPIR-V `OpEntryPoint` and Vulkan descriptor compatibility. Avoid monolithic raw HLSL literal above MSVC's ~16 KiB limit; store original Orbit shader source as separate `.hlsl` files or smaller resource modules, with shader build/dependency tracking and rebuild diagnostics. Never extract/copy private eveshaders.bundle into public assets.

## Resource barriers and lifecycle

- Shared immutable GPU weather/map buffers are `ShaderResource` in renderer, produced/uploaded with transfer fences.
- Light volumes: storage buffer/texture `UnorderedAccess` for compute write, UAV barrier, then `ShaderResource` for view/shadow/atmosphere readers.
- Scene depth: `DepthRead` for cloud and receiver shadow passes. No draw-to-depth unless explicitly scheduled and verified.
- Cloud HDR target: `RenderTarget` for raymarch, `ShaderResource` for temporal composite; alpha alone cannot represent both radiance and physical transmittance if conventions diverge.
- Temporal history: producer writes history A, consumer reads previous B, fence retire and swap only on completed submission; no read/write alias in same frame without explicit resources.
- Reflected/probe view: receives separate low-res representation and cache stamp. Multiview/XR and 16K capture need independent size/budget admission.
- GPU resources are destroyed only after last consumer command-list fence. Project reload, viewport deletion, device loss, shader hot-reload and frame mode change must retire all affected generations.

Use resource state assertions in Vulkan validation layers and output exact VkResult and pipeline name; never show a cloud field as “GPU ready” if the shader pipeline failed to create. There must be a separate `CloudRef readiness` indicator in the existing viewport and Debug Center.

# Temporal upscaling / cloud motion

**EXACT-ARCHIVE** reference release supports low-resolution cloud rendering with temporal upscaling, distance-aware accumulation, orbit mode, cloud motion vectors, overlap/disocclusion improvements and a screenshot mode that disables temporal AA. Its exact reprojection filter source is unavailable. Implement a separate deterministic reference temporal subsystem with:
- Scene camera motion, body rotation, layer wind, local flow map, storm solver advection and macro-representation changes in motion vectors.
- First visible cloud depth / second cloud layer metadata to prevent fog-in-front/opaque-cloud-behind smearing.
- Cloud-integrated radiance and T (separate histories if necessary); optical-depth-aware neighbourhood clamp, reprojection confidence and rejection on disocclusion.
- Epoch discontinuities invalidate history, but normal advection uses motion vectors; no full-cache flush every smooth Sun movement.
- At extremely low FPS, opt to disable temporal accumulation and fall back to spatial reconstruction, preserving user's existing anti-aliasing choices.
- A screenshot supersampling mode should not reuse stale cloud motion history; include same build/source metadata in sidecar.

## Quality/update scheduling policy

Reference profile's horizontal light resolution 224, time slicing 12 and timewarp multiplier 3 are *user-tunable defaults*, not a requirement to dispatch more than current GPU capacity. Add per-module clocks: global weather slow, storm physics with sparse updates, cloud maps when revisions change, light-volume per time slice, rays each rendered frame, temporal resolve each view and event FX when necessary. Timewarp may skip intermediate visual steps, consume event summaries or switch to stored storm replay. Never create unbounded pending GPU updates.

**PROPOSED-PORT** scheduling policy:

```cpp
struct RefFrameBudget {
    u64 maximumResidentBytes=256ull*1024*1024;
    double targetGpuMilliseconds=3.5; // initial policy target, NOT measured performance
    u32 maximumCloudRaySteps=192;
    u32 maximumLightRaySteps=16;
    u32 maximumComputeUpdatesPerFrame=2;
    u32 maximumTimewarpRateMultiplier=3;
};
bool AdmitWork(const RefFrameBudget& budget,
               const ReferenceStageCost& observed,
               const ReferenceRequest& request);
```

Schedule by camera/importance/optical error rather than fixed planet-wide grid. GPU weather/storm simulation memory is a separate physical budget; account for it in total VRAM, but don't double-charge a shared solver to all three renderer implementations. Evict distant scratch light volume before authoritative weather. Degrade gracefully in a documented feature order: temporal quality/resolution, local noise detail, optional god rays, cache cascades, low-priority FX—not core planet/storm location correctness.

# Viewer Debug / Authoring UI

Place under existing viewport Cloud backend selector and Debug Center:
- Backend A / B / EVE-V5-REFERENCE (disabled with a reason until valid).
- Source provenance: release SHA prefix, local asset manifest hash, exact body/config revision, compiled shader fingerprint, physical time, camera, GPU device and render size.
- Visualization tabs: coverage map, type map, flow vectors, Hermite curves, SDF, 3D density, optical depth, Sun cache generation/confidence, sky visibility, receiver shadows, cloud depth, motion vectors, temporal rejection and LOD region.
- Per-pass GPU profiler names under `.EveV5Ref.*`, independent from A and B; samples, cache hit/miss, VRAM, disocclusion%, zero-density rejection, budget-exhausted rays.
- Buttons to export the EXISTING profiler capture and screenshot sidecar, not create a duplicate screenshot/profiler implementation.
- “Asset missing” and “unsupported in reference build” are error statuses, not optional console warnings.

# Runtime transitions

Backend switch A↔B↔EVE must not reset physical simulation. It changes only view-local rendering state; previous history is invalidated for the entered backend if its signature is incompatible. The output shader/resource is not a template instance of A; one owned renderer in separate module, with a single render-graph dispatch contract. If EVE fails, show explicit source/cause and unchanged scene, do not silently render A and record it as a reference result.

# Build and test gates

- Host Windows MSVC build, DXC→SPIR-V shader compile and Vulkan 1.3 validation. Check all shaders declare correct named entrypoints and no descriptor mismatch; CMake compiles new module only when reference backend feature flag is enabled.
- Independent shader reflection/ABI unit tests, 1/2/4/8 layers, empty maps, orbit/ray bounds, cache barriers, overlapping mediums and scene depth correctness.
- Two viewports showing different bodies with different cloud maps: no source pointer reuse or cache reanchor conflict.
- Device reset/project reload/resize hot path: no stale targets or dangling lambda pointers.
- Profiler warm/cold and after timewarp: update count and total GPU time remain within enforced frame budget.
- HDR exposure/eye adaptation must remain the common postprocessor; A/B/reference run with same locked exposure in fidelity tests.
