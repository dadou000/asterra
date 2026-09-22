#pragma once

#include <orbit/lighting/DirectLighting.hpp>
#include <orbit/lighting/LocalLightRegistry.hpp>
#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/volume_fields/VolumeFieldStorage.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <memory>
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

class UniversalVolumeRenderer
{
public:
    UniversalVolumeRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    [[nodiscard]] VolumeRenderRuntimeSettings&
    Settings(
        scene::ObjectId volume);

    [[nodiscard]] VolumeRenderDiagnostics
    Diagnostics(
        scene::ObjectId volume) const noexcept;

    void RemoveMissing(
        const scene::ObjectStore& objects);

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
        bool resetHistory);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view
VolumeRenderDebugModeName(
    VolumeRenderDebugMode mode) noexcept;
} // namespace orbit::volume_render
