#pragma once

#include <orbit/lighting/LightingView.hpp>
#include <orbit/lighting/ScreenSpaceFinalGather.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::lighting
{
struct HybridReflectionSettings
{
    f32 maximumScreenTraceRoughness{0.45F};
    f32 mirrorRoughness{0.08F};
    f32 traceRadiusMeters{40.0F};
    f32 thicknessMeters{0.12F};
    f32 cacheStrength{1.0F};
    u32 maximumSteps{24U};
};

class HybridReflectionRenderer
{
public:
    HybridReflectionRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Resolve(
        rhi::CommandList& commands,
        rhi::Texture& sceneColor,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        // Class 3 (authored proxy) pixels skip the cache fallback.
        rhi::Texture& surfaceEmissionClass,
        rhi::Texture& depth,
        rhi::Buffer& radianceCells,
        rhi::Buffer& radianceLevels,
        u32 radianceLevelCount,
        rhi::Texture& targetSceneColor,
        u32 width,
        u32 height,
        const LightingView& view,
        f32 qualityScale,
        const HybridReflectionSettings& settings = {},
        // The merged mesh distance field: where the screen cannot answer, a
        // smooth surface reflects the room it finds there instead of the
        // radiance cache's low-frequency sky. Null: cache only.
        const SdfGatherInput* sdf = nullptr);

private:
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
    std::unique_ptr<rhi::Buffer> dummySdf_;
};
} // namespace orbit::lighting
