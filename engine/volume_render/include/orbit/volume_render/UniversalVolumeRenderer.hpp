#pragma once

#include <orbit/lighting/DirectLighting.hpp>
#include <orbit/lighting/LocalLightRegistry.hpp>
#include <orbit/lighting/RadianceEstimator.hpp>
#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/volume_fields/VolumeFieldStorage.hpp>
#include <orbit/volume_representation/VolumeRepresentation.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace orbit::volume_render
{
enum class VolumeRenderDebugMode : u8
{
    Composite = 0U,
    Scattering = 1U,
    Extinction = 2U,
    Emission = 3U,
    Shadow = 4U
};

struct VolumeRenderRuntimeSettings
{
    VolumeRenderDebugMode debugMode{
        VolumeRenderDebugMode::Composite};
    bool temporalEnabled{true};

    // M36 Auto representation policy. Authored VolumeRepresentationMode is
    // still the explicit force/debug authority.
    volume_representation::FollowTarget followTarget{
        volume_representation::FollowTarget::AuthoredDomain};
    f64 liveDistanceMeters{120.0};
    f64 passiveDistanceMeters{1200.0};
    f32 liveProjectedPixels{96.0F};
    f32 passiveProjectedPixels{12.0F};
    f32 hysteresisFraction{0.12F};
    u32 coarseResolution{24U};
    u32 passiveResolution{8U};
    u32 coarseRaymarchSteps{24U};
    u32 passiveRaymarchSteps{8U};
};

struct VolumeRenderDiagnostics
{
    bool rendered{false};
    bool historyValid{false};
    u32 raymarchSteps{0U};
    u32 shadowSteps{0U};
    u32 localLightCount{0U};
    u32 residentTiles{0U};
    u64 historyBytes{0U};
    u64 scratchBytes{0U};

    volume_representation::ResolvedRepresentation representation{
        volume_representation::ResolvedRepresentation::Live};
    volume_representation::ResolvedRepresentation previousRepresentation{
        volume_representation::ResolvedRepresentation::Live};
    f32 liveWeight{1.0F};
    f32 coarseWeight{0.0F};
    f32 passiveWeight{0.0F};
    f64 distanceToBoundsMeters{0.0};
    f32 projectedDiameterPixels{0.0F};
    bool representationForced{false};
    bool representationTransition{false};
    bool denseFieldRequired{true};
    bool bakedFallback{false};
    u64 stableAddressFingerprint{0U};
};

struct HomogeneousVolumeReference
{
    f32 transmittance{1.0F};
    math::Float3 scatteredRadiance{};
    math::Float3 emittedRadiance{};
    math::Float3 totalRadiance{};
};

[[nodiscard]] HomogeneousVolumeReference
IntegrateHomogeneousVolume(
    f32 density,
    f32 distanceMeters,
    f32 extinctionScale,
    f32 singleScatteringAlbedo,
    math::Float3 scatteringColor,
    math::Float3 incidentRadiance,
    math::Float3 emissionColor,
    f32 emissionScalar,
    f32 emissionScale) noexcept;

[[nodiscard]] f32
HenyeyGreensteinPhase(
    f32 cosineTheta,
    f32 anisotropy) noexcept;

[[nodiscard]] std::optional<
    lighting::EmissiveVolumeSource>
BuildEmissiveVolumeSource(
    const world_model::ResolvedVolumeDomain& domain,
    f32 emissionAuthorityScalar) noexcept;

class UniversalVolumeRenderer
{
public:
    UniversalVolumeRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    [[nodiscard]] VolumeRenderRuntimeSettings&
    Settings(
        scene::ObjectId volume);

#ifndef ORBIT_VOLUME_RENDER_BASE_IMPLEMENTATION
    [[nodiscard]] VolumeRenderDiagnostics
    Diagnostics(
        scene::ObjectId volume) const noexcept;
#else
    [[nodiscard]] VolumeRenderDiagnostics
    LiveDiagnostics(
        scene::ObjectId volume) const noexcept;
#endif

    void RemoveMissing(
        const scene::ObjectStore& objects);

#ifndef ORBIT_VOLUME_RENDER_BASE_IMPLEMENTATION
    void AddPasses(
        render_graph::RenderGraph& graph,
        std::string_view prefix,
        std::string_view viewportId,
        render_graph::TextureHandle sceneColor,
        render_graph::TextureHandle depth,
        u32 width,
        u32 height,
        const render_view::CameraState& camera,
        const lighting::LightingView& lightingView,
        const world_model::ResolvedVolumeDomain& domain,
        volume_fields::VolumeFieldStorage& storage,
        const volume_fields::ImportedVolumeFields& fields,
        const lighting::DirectionalLight& stellar,
        std::span<const lighting::ResolvedLocalLight> localLights,
        render_graph::BufferHandle radianceCells,
        render_graph::BufferHandle radianceLevels,
        u32 radianceLevelCount,
        bool resetHistory);
#else
    void AddLivePasses(
        render_graph::RenderGraph& graph,
        std::string_view prefix,
        std::string_view viewportId,
        render_graph::TextureHandle sceneColor,
        render_graph::TextureHandle depth,
        u32 width,
        u32 height,
        const render_view::CameraState& camera,
        const lighting::LightingView& lightingView,
        const world_model::ResolvedVolumeDomain& domain,
        volume_fields::VolumeFieldStorage& storage,
        const volume_fields::ImportedVolumeFields& fields,
        const lighting::DirectionalLight& stellar,
        std::span<const lighting::ResolvedLocalLight> localLights,
        render_graph::BufferHandle radianceCells,
        render_graph::BufferHandle radianceLevels,
        u32 radianceLevelCount,
        bool resetHistory);
#endif

    // Available to the M36 policy wrapper; this is the unchanged M35 backend.
#ifndef ORBIT_VOLUME_RENDER_BASE_IMPLEMENTATION
    [[nodiscard]] VolumeRenderDiagnostics
    LiveDiagnostics(
        scene::ObjectId volume) const noexcept;

    void AddLivePasses(
        render_graph::RenderGraph& graph,
        std::string_view prefix,
        std::string_view viewportId,
        render_graph::TextureHandle sceneColor,
        render_graph::TextureHandle depth,
        u32 width,
        u32 height,
        const render_view::CameraState& camera,
        const lighting::LightingView& lightingView,
        const world_model::ResolvedVolumeDomain& domain,
        volume_fields::VolumeFieldStorage& storage,
        const volume_fields::ImportedVolumeFields& fields,
        const lighting::DirectionalLight& stellar,
        std::span<const lighting::ResolvedLocalLight> localLights,
        render_graph::BufferHandle radianceCells,
        render_graph::BufferHandle radianceLevels,
        u32 radianceLevelCount,
        bool resetHistory);
#endif

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view
VolumeRenderDebugModeName(
    VolumeRenderDebugMode mode) noexcept;
} // namespace orbit::volume_render
