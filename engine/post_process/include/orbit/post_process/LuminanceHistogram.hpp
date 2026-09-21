#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <array>
#include <memory>
#include <span>

namespace orbit::post_process
{
inline constexpr u32 kLuminanceHistogramBins = 128U;

struct LuminanceHistogramConfig
{
    f32 minimumLog2{-16.0F};
    f32 maximumLog2{16.0F};

    // 0 = uniform full-frame weighting, 1 = fully center-weighted.
    f32 centerWeightStrength{0.25F};
    f32 centerWeightRadius{0.70F};
};

struct LuminanceHistogramStatistics
{
    u32 sampleCount{0U};
    u32 weightedSampleCount{0U};
    u32 peakBin{0U};
    bool valid{false};

    f32 medianLog2{0.0F};
    f32 p90Log2{0.0F};
    f32 p95Log2{0.0F};
    f32 p99Log2{0.0F};

    f32 medianLuminance{0.0F};
    f32 p90Luminance{0.0F};
    f32 p95Luminance{0.0F};
    f32 p99Luminance{0.0F};

    f32 peakLog2{0.0F};
    f32 peakLuminance{0.0F};
    f32 meanLog2{0.0F};
    f32 geometricMeanLuminance{0.0F};
};

// Exact GPU readback ABI. All values are scalar 32-bit fields so HLSL
// ByteAddressBuffer offsets remain explicit and backend-independent.
struct GpuLuminanceHistogramStatistics
{
    u32 sampleCount{0U};
    u32 weightedSampleCount{0U};
    u32 peakBin{0U};
    u32 valid{0U};

    f32 medianLog2{0.0F};
    f32 p90Log2{0.0F};
    f32 p95Log2{0.0F};
    f32 p99Log2{0.0F};

    f32 medianLuminance{0.0F};
    f32 p90Luminance{0.0F};
    f32 p95Luminance{0.0F};
    f32 p99Luminance{0.0F};

    f32 peakLog2{0.0F};
    f32 peakLuminance{0.0F};
    f32 meanLog2{0.0F};
    f32 geometricMeanLuminance{0.0F};
};

static_assert(
    sizeof(GpuLuminanceHistogramStatistics) == 64U);

[[nodiscard]] LuminanceHistogramStatistics
AnalyzeLuminanceHistogram(
    std::span<const u32> weightedBins,
    u32 sampleCount,
    const LuminanceHistogramConfig& config = {});

[[nodiscard]] LuminanceHistogramStatistics
DecodeLuminanceHistogramStatistics(
    const GpuLuminanceHistogramStatistics& gpu) noexcept;

class LuminanceHistogramRenderer
{
public:
    LuminanceHistogramRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Reset(
        rhi::CommandList& commands,
        rhi::Buffer& histogram,
        rhi::Buffer& statistics);

    void Build(
        rhi::CommandList& commands,
        rhi::Texture& sourceHdr,
        rhi::Texture& meteringMask,
        rhi::Buffer& histogram,
        rhi::Buffer& statistics,
        u32 width,
        u32 height,
        const LuminanceHistogramConfig& config = {});

    void Reduce(
        rhi::CommandList& commands,
        rhi::Buffer& histogram,
        rhi::Buffer& statistics,
        const LuminanceHistogramConfig& config = {});

private:
    std::unique_ptr<rhi::ComputePipeline> resetPipeline_;
    std::unique_ptr<rhi::ComputePipeline> histogramPipeline_;
    std::unique_ptr<rhi::ComputePipeline> reducePipeline_;
};
} // namespace orbit::post_process
