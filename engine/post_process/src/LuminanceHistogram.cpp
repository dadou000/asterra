#include <orbit/post_process/LuminanceHistogram.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::post_process
{
namespace
{
[[nodiscard]] f32 BinLogCenter(
    const u32 bin,
    const LuminanceHistogramConfig& config) noexcept
{
    const f32 range =
        std::max(
            config.maximumLog2 -
                config.minimumLog2,
            1.0e-4F);

    return
        config.minimumLog2 +
        (static_cast<f32>(bin) + 0.5F) /
            static_cast<f32>(
                kLuminanceHistogramBins) *
            range;
}

[[nodiscard]] u32 PercentileBin(
    const std::span<const u32> bins,
    const u64 totalWeight,
    const f64 percentile) noexcept
{
    if (bins.empty() ||
        totalWeight == 0U)
    {
        return 0U;
    }

    const u64 target =
        std::max<u64>(
            1U,
            static_cast<u64>(
                std::ceil(
                    static_cast<f64>(
                        totalWeight) *
                    std::clamp(
                        percentile,
                        0.0,
                        1.0))));

    u64 cumulative = 0U;

    for (u32 index = 0U;
         index < bins.size();
         ++index)
    {
        cumulative += bins[index];

        if (cumulative >= target)
        {
            return index;
        }
    }

    return
        static_cast<u32>(
            bins.size() - 1U);
}

constexpr const char* kOverlayVs = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 p[6] =
    {
        float2(-1,-1), float2(-1,1), float2(1,-1),
        float2(1,-1), float2(-1,1), float2(1,1)
    };
    const float2 uv[6] =
    {
        float2(0,1), float2(0,0), float2(1,1),
        float2(1,1), float2(0,0), float2(1,0)
    };

    VSOutput output;
    output.position = float4(p[vertexId], 0, 1);
    output.uv = uv[vertexId];
    return output;
}
)";

constexpr const char* kOverlayPs = R"(
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_mask;
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_maskSampler;

struct Constants
{
    float opacity;
    float3 padding;
};
[[vk::push_constant]] Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target0
{
    const float4 mask =
        g_mask.Sample(
            g_maskSampler,
            input.uv);

    const float weight =
        saturate(mask.r);
    const float normalizedLog =
        saturate(mask.g);

    const float3 lowColor =
        float3(0.05, 0.35, 1.0);
    const float3 highColor =
        float3(1.0, 0.22, 0.06);

    const float3 color =
        lerp(
            lowColor,
            highColor,
            normalizedLog);

    return float4(
        color,
        saturate(g.opacity) *
            (0.18 + 0.82 * weight));
}
)";

constexpr const char* kResetCs = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_histogram : register(u0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_statistics : register(u1);

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint index = dispatchId.x;

    if (index < 128u)
    {
        g_histogram.Store(index * 4u, 0u);
    }

    if (index < 16u)
    {
        g_statistics.Store(index * 4u, 0u);
    }
}
)";

constexpr const char* kHistogramCs = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_histogram : register(u0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_statistics : register(u1);

[[vk::binding(2, 0)]]
RWTexture2D<float4> g_meteringMask : register(u2);

[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_source : register(t3);
[[vk::binding(3, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sourceSampler : register(s3);

struct Constants
{
    uint width;
    uint height;
    float minimumLog2;
    float maximumLog2;

    float centerWeightStrength;
    float centerWeightRadius;
    uint bins;
    uint fixedWeightScale;
};

[[vk::push_constant]]
Constants g;

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g.width ||
        dispatchId.y >= g.height)
    {
        return;
    }

    const uint2 pixel =
        dispatchId.xy;

    const float2 uv =
        (float2(pixel) + 0.5) /
        float2(g.width, g.height);

    const float3 scene =
        max(
            g_source.SampleLevel(
                g_sourceSampler,
                uv,
                0).rgb,
            0.0);

    const float luminance =
        dot(
            scene,
            float3(
                0.2126,
                0.7152,
                0.0722));

    const float logLum =
        log2(
            max(
                luminance,
                exp2(g.minimumLog2)));

    const float range =
        max(
            g.maximumLog2 -
                g.minimumLog2,
            1.0e-4);

    const float normalized =
        saturate(
            (logLum -
             g.minimumLog2) /
            range);

    const uint bin =
        min(
            uint(
                normalized *
                float(g.bins)),
            g.bins - 1u);

    const float2 centered =
        uv * 2.0 - 1.0;

    const float radius =
        max(
            g.centerWeightRadius,
            0.05);

    const float centerWeight =
        exp(
            -dot(centered, centered) /
            (radius * radius));

    const float weight =
        lerp(
            1.0,
            centerWeight,
            saturate(
                g.centerWeightStrength));

    const uint fixedWeight =
        max(
            1u,
            uint(
                round(
                    weight *
                    float(g.fixedWeightScale))));

    uint ignored;

    g_histogram.InterlockedAdd(
        bin * 4u,
        fixedWeight,
        ignored);

    g_statistics.InterlockedAdd(
        0u,
        1u,
        ignored);

    g_statistics.InterlockedAdd(
        4u,
        fixedWeight,
        ignored);

    g_statistics.InterlockedMax(
        8u,
        bin,
        ignored);

    g_meteringMask[pixel] =
        float4(
            weight,
            normalized,
            luminance,
            1.0);
}
)";

constexpr const char* kReduceCs = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_histogram : register(t0);

[[vk::binding(1, 0)]]
RWByteAddressBuffer g_statistics : register(u1);

struct Constants
{
    float minimumLog2;
    float maximumLog2;
    uint bins;
    uint reserved;
};

[[vk::push_constant]]
Constants g;

float BinLogCenter(uint bin)
{
    const float range =
        max(
            g.maximumLog2 -
                g.minimumLog2,
            1.0e-4);

    return
        g.minimumLog2 +
        (float(bin) + 0.5) /
            float(g.bins) *
            range;
}

[numthreads(1, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    const uint totalWeight =
        g_statistics.Load(4u);

    const uint peakBin =
        g_statistics.Load(8u);

    if (totalWeight == 0u)
    {
        g_statistics.Store(12u, 0u);
        return;
    }

    const uint targets[4] = {
        max(1u, uint(ceil(float(totalWeight) * 0.50))),
        max(1u, uint(ceil(float(totalWeight) * 0.90))),
        max(1u, uint(ceil(float(totalWeight) * 0.95))),
        max(1u, uint(ceil(float(totalWeight) * 0.99)))
    };

    uint foundBins[4] = {
        g.bins - 1u,
        g.bins - 1u,
        g.bins - 1u,
        g.bins - 1u
    };

    bool found[4] = {
        false, false, false, false
    };

    uint cumulative = 0u;
    float weightedLogSum = 0.0;

    [loop]
    for (uint bin = 0u;
         bin < g.bins;
         ++bin)
    {
        const uint count =
            g_histogram.Load(
                bin * 4u);

        if (count == 0u)
        {
            continue;
        }

        cumulative += count;
        weightedLogSum +=
            BinLogCenter(bin) *
            float(count);

        [unroll]
        for (uint p = 0u;
             p < 4u;
             ++p)
        {
            if (!found[p] &&
                cumulative >= targets[p])
            {
                found[p] = true;
                foundBins[p] = bin;
            }
        }
    }

    const float p50 =
        BinLogCenter(foundBins[0]);
    const float p90 =
        BinLogCenter(foundBins[1]);
    const float p95 =
        BinLogCenter(foundBins[2]);
    const float p99 =
        BinLogCenter(foundBins[3]);
    const float peak =
        BinLogCenter(
            min(
                peakBin,
                g.bins - 1u));

    const float meanLog =
        weightedLogSum /
        max(float(totalWeight), 1.0);

    g_statistics.Store(12u, 1u);

    g_statistics.Store(16u, asuint(p50));
    g_statistics.Store(20u, asuint(p90));
    g_statistics.Store(24u, asuint(p95));
    g_statistics.Store(28u, asuint(p99));

    g_statistics.Store(32u, asuint(exp2(p50)));
    g_statistics.Store(36u, asuint(exp2(p90)));
    g_statistics.Store(40u, asuint(exp2(p95)));
    g_statistics.Store(44u, asuint(exp2(p99)));

    g_statistics.Store(48u, asuint(peak));
    g_statistics.Store(52u, asuint(exp2(peak)));
    g_statistics.Store(56u, asuint(meanLog));
    g_statistics.Store(60u, asuint(exp2(meanLog)));
}
)";
} // namespace

LuminanceHistogramStatistics
AnalyzeLuminanceHistogram(
    const std::span<const u32> weightedBins,
    const u32 sampleCount,
    const LuminanceHistogramConfig& config)
{
    if (weightedBins.size() !=
        kLuminanceHistogramBins)
    {
        throw std::invalid_argument(
            "Luminance histogram requires exactly 128 bins.");
    }

    if (!std::isfinite(config.minimumLog2) ||
        !std::isfinite(config.maximumLog2) ||
        config.maximumLog2 <= config.minimumLog2)
    {
        throw std::invalid_argument(
            "Luminance histogram log range is invalid.");
    }

    u64 totalWeight = 0U;
    u32 peakBin = 0U;
    f64 weightedLogSum = 0.0;

    for (u32 bin = 0U;
         bin < weightedBins.size();
         ++bin)
    {
        const u32 count =
            weightedBins[bin];

        totalWeight += count;

        if (count > 0U)
        {
            peakBin = bin;
        }

        weightedLogSum +=
            static_cast<f64>(count) *
            static_cast<f64>(
                BinLogCenter(
                    bin,
                    config));
    }

    if (totalWeight == 0U)
    {
        return {};
    }

    const u32 p50Bin =
        PercentileBin(
            weightedBins,
            totalWeight,
            0.50);
    const u32 p90Bin =
        PercentileBin(
            weightedBins,
            totalWeight,
            0.90);
    const u32 p95Bin =
        PercentileBin(
            weightedBins,
            totalWeight,
            0.95);
    const u32 p99Bin =
        PercentileBin(
            weightedBins,
            totalWeight,
            0.99);

    const f32 p50 =
        BinLogCenter(p50Bin, config);
    const f32 p90 =
        BinLogCenter(p90Bin, config);
    const f32 p95 =
        BinLogCenter(p95Bin, config);
    const f32 p99 =
        BinLogCenter(p99Bin, config);
    const f32 peak =
        BinLogCenter(peakBin, config);
    const f32 meanLog =
        static_cast<f32>(
            weightedLogSum /
            static_cast<f64>(
                totalWeight));

    return {
        .sampleCount = sampleCount,
        .weightedSampleCount =
            static_cast<u32>(
                std::min<u64>(
                    totalWeight,
                    std::numeric_limits<u32>::max())),
        .peakBin = peakBin,
        .valid = true,
        .medianLog2 = p50,
        .p90Log2 = p90,
        .p95Log2 = p95,
        .p99Log2 = p99,
        .medianLuminance =
            std::exp2(p50),
        .p90Luminance =
            std::exp2(p90),
        .p95Luminance =
            std::exp2(p95),
        .p99Luminance =
            std::exp2(p99),
        .peakLog2 = peak,
        .peakLuminance =
            std::exp2(peak),
        .meanLog2 = meanLog,
        .geometricMeanLuminance =
            std::exp2(meanLog)
    };
}

LuminanceHistogramStatistics
DecodeLuminanceHistogramStatistics(
    const GpuLuminanceHistogramStatistics& gpu) noexcept
{
    return {
        .sampleCount = gpu.sampleCount,
        .weightedSampleCount =
            gpu.weightedSampleCount,
        .peakBin = gpu.peakBin,
        .valid = gpu.valid != 0U,
        .medianLog2 = gpu.medianLog2,
        .p90Log2 = gpu.p90Log2,
        .p95Log2 = gpu.p95Log2,
        .p99Log2 = gpu.p99Log2,
        .medianLuminance =
            gpu.medianLuminance,
        .p90Luminance =
            gpu.p90Luminance,
        .p95Luminance =
            gpu.p95Luminance,
        .p99Luminance =
            gpu.p99Luminance,
        .peakLog2 = gpu.peakLog2,
        .peakLuminance =
            gpu.peakLuminance,
        .meanLog2 = gpu.meanLog2,
        .geometricMeanLuminance =
            gpu.geometricMeanLuminance
    };
}

LuminanceHistogramRenderer::
LuminanceHistogramRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto reset =
        compiler.Compile({
            .source = kResetCs,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    resetPipeline_ =
        device.CreateComputePipeline({
            .computeShader = {
                .data = reset.bytecode.data(),
                .size = reset.bytecode.size()
            },
            .pushConstantDwords = 0U,
            .shaderResourceBuffers = 2U
        });

    const auto histogram =
        compiler.Compile({
            .source = kHistogramCs,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    histogramPipeline_ =
        device.CreateComputePipeline({
            .computeShader = {
                .data = histogram.bytecode.data(),
                .size = histogram.bytecode.size()
            },
            .pushConstantDwords = 8U,
            .shaderResourceBuffers = 2U,
            .storageTextures = 1U,
            .sampledTextures = 1U
        });

    const auto reduce =
        compiler.Compile({
            .source = kReduceCs,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    reducePipeline_ =
        device.CreateComputePipeline({
            .computeShader = {
                .data = reduce.bytecode.data(),
                .size = reduce.bytecode.size()
            },
            .pushConstantDwords = 4U,
            .shaderResourceBuffers = 2U
        });

    const auto overlayVs =
        compiler.Compile({
            .source = kOverlayVs,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });

    const auto overlayPs =
        compiler.Compile({
            .source = kOverlayPs,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    overlayPipeline_ =
        device.CreateGraphicsPipeline({
            .vertexShader = {
                .data = overlayVs.bytecode.data(),
                .size = overlayVs.bytecode.size()
            },
            .pixelShader = {
                .data = overlayPs.bytecode.data(),
                .size = overlayPs.bytecode.size()
            },
            .vertexAttributes = {},
            .vertexStrideBytes = 0U,
            .pushConstantDwords = 4U,
            .shaderResourceBuffers = 0U,
            .sampledTextures = 1U,
            .topology =
                rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Alpha,
            .depthTest = false,
            .depthWrite = false,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA8_UNorm
            },
            .colorAttachmentCount = 1U
        });
}

void LuminanceHistogramRenderer::Reset(
    rhi::CommandList& commands,
    rhi::Buffer& histogram,
    rhi::Buffer& statistics)
{
    commands.SetComputePipeline(
        *resetPipeline_);
    commands.SetComputeBuffer(
        0U,
        histogram);
    commands.SetComputeBuffer(
        1U,
        statistics);
    commands.Dispatch(
        2U,
        1U,
        1U);
}

void LuminanceHistogramRenderer::Build(
    rhi::CommandList& commands,
    rhi::Texture& sourceHdr,
    rhi::Texture& meteringMask,
    rhi::Buffer& histogram,
    rhi::Buffer& statistics,
    const u32 width,
    const u32 height,
    const LuminanceHistogramConfig& config)
{
    if (width == 0U ||
        height == 0U)
    {
        return;
    }

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const std::array<u32, 8> constants{
        width,
        height,
        bits(config.minimumLog2),
        bits(config.maximumLog2),
        bits(std::clamp(
            config.centerWeightStrength,
            0.0F,
            1.0F)),
        bits(std::max(
            config.centerWeightRadius,
            0.05F)),
        kLuminanceHistogramBins,
        256U
    };

    commands.SetComputePipeline(
        *histogramPipeline_);
    commands.SetComputeConstants(
        constants);

    commands.SetComputeBuffer(
        0U,
        histogram);
    commands.SetComputeBuffer(
        1U,
        statistics);
    commands.SetComputeStorageTexture(
        0U,
        meteringMask);
    commands.SetComputeTexture(
        0U,
        sourceHdr);

    commands.Dispatch(
        (width + 7U) / 8U,
        (height + 7U) / 8U,
        1U);
}

void LuminanceHistogramRenderer::Reduce(
    rhi::CommandList& commands,
    rhi::Buffer& histogram,
    rhi::Buffer& statistics,
    const LuminanceHistogramConfig& config)
{
    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const std::array<u32, 4> constants{
        bits(config.minimumLog2),
        bits(config.maximumLog2),
        kLuminanceHistogramBins,
        0U
    };

    commands.SetComputePipeline(
        *reducePipeline_);
    commands.SetComputeConstants(
        constants);
    commands.SetComputeBuffer(
        0U,
        histogram);
    commands.SetComputeBuffer(
        1U,
        statistics);
    commands.Dispatch(
        1U,
        1U,
        1U);
}

void LuminanceHistogramRenderer::DrawMeteringOverlay(
    rhi::CommandList& commands,
    rhi::Texture& meteringMask,
    rhi::Texture& targetDisplay,
    const u32 width,
    const u32 height,
    const f32 opacity)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    const std::array<u32, 4> constants{
        std::bit_cast<u32>(
            std::clamp(
                opacity,
                0.0F,
                1.0F)),
        0U,
        0U,
        0U
    };

    commands.SetRenderTarget(
        targetDisplay);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<f32>(width),
        .height = static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    });
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right = static_cast<i32>(width),
        .bottom = static_cast<i32>(height)
    });

    commands.SetGraphicsPipeline(
        *overlayPipeline_);
    commands.SetGraphicsConstants(
        constants);
    commands.SetGraphicsTexture(
        0U,
        meteringMask);
    commands.Draw(6U);
}

} // namespace orbit::post_process
