#pragma once

#include <orbit/celestial_clouds/CloudField.hpp>
#include <orbit/celestial_clouds/UnifiedCloudFoundation.hpp>
#include <orbit/celestial_atmosphere/AtmosphereRenderer.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::celestial_clouds::unified
{
// System B's independent, deliberately bounded first GPU stage. It owns its
// shader pipelines and never calls, imports, or mutates CloudRenderer (A).
// B1 draws a physical shell from the shared authored weather map. Advanced
// storm-grid density, long-range optical cache and temporal reconstruction
// are later stages, not implied by this API.
class UnifiedCloudRenderer final
{
public:
    UnifiedCloudRenderer(rhi::Device& device, const shader::Compiler& compiler);
    ~UnifiedCloudRenderer();

    UnifiedCloudRenderer(const UnifiedCloudRenderer&) = delete;
    UnifiedCloudRenderer& operator=(const UnifiedCloudRenderer&) = delete;

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& depth,
        GpuCloudFieldProduct& field,
        rhi::Texture& target,
        f64 bodyRadiusMeters,
        const CloudLayerParameters& layer,
        const celestial_atmosphere::AtmosphereRenderView& view);

    void DrawReceiverShadow(
        rhi::CommandList& commands,
        rhi::Texture& depth,
        GpuCloudFieldProduct& field,
        rhi::Texture& target,
        f64 bodyRadiusMeters,
        const CloudLayerParameters& layer,
        const celestial_atmosphere::AtmosphereRenderView& view);

    void Composite(
        rhi::CommandList& commands,
        rhi::Texture& cloudMedium,
        rhi::Texture& scene,
        u32 width, u32 height);

private:
    void DrawMedium(
        rhi::CommandList& commands, bool shadow,
        rhi::Texture& depth, GpuCloudFieldProduct& field, rhi::Texture& target,
        f64 bodyRadiusMeters, const CloudLayerParameters& layer,
        const celestial_atmosphere::AtmosphereRenderView& view);

    std::unique_ptr<rhi::GraphicsPipeline> march_;
    std::unique_ptr<rhi::GraphicsPipeline> receiverShadow_;
    std::unique_ptr<rhi::GraphicsPipeline> composite_;
};
} // namespace orbit::celestial_clouds::unified
