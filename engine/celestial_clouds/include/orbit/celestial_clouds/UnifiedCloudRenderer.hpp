#pragma once

#include <orbit/celestial_clouds/CloudField.hpp>
#include <orbit/celestial_clouds/UnifiedCloudFoundation.hpp>
#include <orbit/celestial_atmosphere/AtmosphereRenderer.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>
#include <cstdint>

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

    // B3 cache belongs to exactly one viewport, never to A's light volume.
    // First cascade: 32x32 columns at 2km and 16 vertical slabs; one
    // altitude slab is refreshed each frame (bounded GPU work).
    struct SunCache
    {
        std::unique_ptr<rhi::Buffer> buffer;
        math::Double3 anchor{};
        bool anchored{false};
        u32 generation{1U};
        u32 phase{0U};
        math::Float3 lastSun{};
        bool sunSet{false};
        u64 sourceFingerprint{0U};
        const rhi::Buffer* lastStorm{nullptr};
        f64 lastRadius{0.0};
        bool active{false};
        bool initialized{false};
    };

    [[nodiscard]] std::unique_ptr<SunCache> CreateSunCache(rhi::Device& device) const;
    void UpdateSunCache(
        rhi::CommandList& commands,
        SunCache& cache,
        GpuCloudFieldProduct& field,
        const CloudFieldProduct& source,
        f64 referenceRadiusMeters,
        const celestial_atmosphere::AtmosphereRenderView& view,
        rhi::Buffer* stormExport);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& depth,
        GpuCloudFieldProduct& field,
        rhi::Texture& target,
        f64 bodyRadiusMeters,
        const CloudFieldProduct& source,
        const celestial_atmosphere::AtmosphereRenderView& view,
        rhi::Buffer* stormExport,
        const SunCache* sunCache = nullptr);

    void DrawReceiverShadow(
        rhi::CommandList& commands,
        rhi::Texture& depth,
        GpuCloudFieldProduct& field,
        rhi::Texture& target,
        f64 bodyRadiusMeters,
        const CloudFieldProduct& source,
        const celestial_atmosphere::AtmosphereRenderView& view,
        rhi::Buffer* stormExport);

    void Composite(
        rhi::CommandList& commands,
        rhi::Texture& cloudMedium,
        rhi::Texture& scene,
        u32 width, u32 height);

private:
    void DrawMedium(
        rhi::CommandList& commands, bool shadow,
        rhi::Texture& depth, GpuCloudFieldProduct& field, rhi::Texture& target,
        f64 bodyRadiusMeters, const CloudFieldProduct& source,
        const celestial_atmosphere::AtmosphereRenderView& view,
        rhi::Buffer* stormExport,
        const SunCache* sunCache);

    std::unique_ptr<rhi::ComputePipeline> sunCachePipeline_;
    std::unique_ptr<rhi::ComputePipeline> sunCacheClearPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> march_;
    std::unique_ptr<rhi::GraphicsPipeline> receiverShadow_;
    std::unique_ptr<rhi::GraphicsPipeline> composite_;
    // B-owned inactive placeholder; never reuses A's mutable storm binding.
    std::unique_ptr<rhi::Buffer> emptyStorm_;
    std::unique_ptr<rhi::Buffer> emptySunCache_;
};
} // namespace orbit::celestial_clouds::unified
