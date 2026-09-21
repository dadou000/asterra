#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::post_process
{
enum class HighlightDebugMode : u8
{
    Composite = 0U,
    BloomExtraction = 1U,
    GlareExtraction = 2U,
    FlareExtraction = 3U
};

struct HighlightEffectsConfig
{
    bool bloomEnabled{true};
    bool glareEnabled{true};
    bool flareEnabled{true};

    // All thresholds are in exposed scene-linear units. The photopic ceiling
    // is supplied separately so authored calibration remains in eye state.
    f32 bloomThreshold{1.0F};
    f32 bloomKnee{0.50F};
    f32 bloomStrength{0.08F};
    f32 bloomRadiusPixels{6.0F};

    f32 glareThreshold{2.5F};
    f32 glareStrength{0.035F};
    f32 glareRadiusPixels{18.0F};

    f32 flareThreshold{5.0F};
    f32 flareStrength{0.025F};
    f32 flareCompactness{2.0F};
    f32 flareGhostScale{0.35F};

    HighlightDebugMode debugMode{
        HighlightDebugMode::Composite};
};

struct HighlightClassification
{
    f32 bloom{0.0F};
    f32 glare{0.0F};
    f32 flare{0.0F};
};

[[nodiscard]] HighlightClassification
ClassifyHighlight(
    f32 exposedLuminance,
    f32 neighborhoodLuminance,
    const HighlightEffectsConfig& config = {}) noexcept;

class HighlightEffectsRenderer
{
public:
    HighlightEffectsRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& sourceHdr,
        rhi::Texture& targetDisplayLinear,
        u32 width,
        u32 height,
        f32 exposureScale,
        bool toneMapEnabled,
        const HighlightEffectsConfig& config = {});

private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
};
} // namespace orbit::post_process
