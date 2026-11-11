#include <orbit/post_process/AntiAliasing.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <string>

namespace orbit::post_process
{
namespace
{
constexpr u32 kFxaaPushDwords = 4U;
constexpr u32 kTaaPushDwords = 32U;

// FXAA 3.11-style edge anti-aliasing. Edge detection runs on the luma of the
// Karis-compressed colour (perceptual-ish, immune to HDR highlights); the
// final tap reads the HDR source.
constexpr const char* kFxaaShader = R"(
[[vk::binding(0, 0)]]
RWTexture2D<float4> g_destination : register(u0);

[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] Texture2D g_source;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] SamplerState g_sourceSampler;

struct Constants
{
    uint width;
    uint height;
    uint pad0;
    uint pad1;
};
[[vk::push_constant]] Constants g;

float Luma(float3 c)
{
    c = max(c, 0.0);
    c = c / (1.0 + max(c.r, max(c.g, c.b)));
    return sqrt(dot(c, float3(0.299, 0.587, 0.114)));
}

float LumaAt(float2 uv)
{
    return Luma(g_source.SampleLevel(g_sourceSampler, uv, 0).rgb);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g.width || id.y >= g.height)
    {
        return;
    }

    const float2 texel = 1.0 / float2(g.width, g.height);
    const float2 uv = (float2(id.xy) + 0.5) * texel;
    const float4 centerColor = g_source.SampleLevel(g_sourceSampler, uv, 0);

    const float lumaM = Luma(centerColor.rgb);
    const float lumaN = LumaAt(uv + float2(0.0, -texel.y));
    const float lumaS = LumaAt(uv + float2(0.0, texel.y));
    const float lumaW = LumaAt(uv + float2(-texel.x, 0.0));
    const float lumaE = LumaAt(uv + float2(texel.x, 0.0));

    const float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    const float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    const float range = lumaMax - lumaMin;

    if (range < max(0.0312, lumaMax * 0.125))
    {
        g_destination[id.xy] = centerColor;
        return;
    }

    const float lumaNW = LumaAt(uv + float2(-texel.x, -texel.y));
    const float lumaNE = LumaAt(uv + float2(texel.x, -texel.y));
    const float lumaSW = LumaAt(uv + float2(-texel.x, texel.y));
    const float lumaSE = LumaAt(uv + float2(texel.x, texel.y));

    // Sub-pixel aliasing: how far the centre sits from its 4-neighbour mean.
    const float lumaAverage = (lumaN + lumaS + lumaW + lumaE) * 0.25;
    float subpixel = saturate((abs(lumaAverage - lumaM) / range - 0.25) * (1.0 / 0.75));
    subpixel = smoothstep(0.0, 1.0, subpixel);
    subpixel = subpixel * subpixel * 0.75;

    const float edgeHorizontal =
        abs(-2.0 * lumaN + lumaNW + lumaNE) +
        2.0 * abs(-2.0 * lumaM + lumaW + lumaE) +
        abs(-2.0 * lumaS + lumaSW + lumaSE);
    const float edgeVertical =
        abs(-2.0 * lumaW + lumaNW + lumaSW) +
        2.0 * abs(-2.0 * lumaM + lumaN + lumaS) +
        abs(-2.0 * lumaE + lumaNE + lumaSE);
    const bool horizontal = edgeHorizontal >= edgeVertical;

    float luma1 = horizontal ? lumaN : lumaW;
    float luma2 = horizontal ? lumaS : lumaE;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;
    const bool firstSteeper = abs(gradient1) >= abs(gradient2);
    const float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

    float stepLength = horizontal ? texel.y : texel.x;
    float lumaLocalAverage;
    if (firstSteeper)
    {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaM);
    }
    else
    {
        lumaLocalAverage = 0.5 * (luma2 + lumaM);
    }

    float2 edgeUv = uv;
    if (horizontal)
    {
        edgeUv.y += stepLength * 0.5;
    }
    else
    {
        edgeUv.x += stepLength * 0.5;
    }

    const float2 along = horizontal ? float2(texel.x, 0.0) : float2(0.0, texel.y);
    float2 uvNegative = edgeUv - along;
    float2 uvPositive = edgeUv + along;
    float endNegative = LumaAt(uvNegative) - lumaLocalAverage;
    float endPositive = LumaAt(uvPositive) - lumaLocalAverage;
    bool doneNegative = abs(endNegative) >= gradientScaled;
    bool donePositive = abs(endPositive) >= gradientScaled;

    [unroll]
    for (int i = 2; i < 10; ++i)
    {
        if (doneNegative && donePositive)
        {
            break;
        }
        if (!doneNegative)
        {
            uvNegative -= along * (i < 6 ? 1.0 : 2.0);
            endNegative = LumaAt(uvNegative) - lumaLocalAverage;
            doneNegative = abs(endNegative) >= gradientScaled;
        }
        if (!donePositive)
        {
            uvPositive += along * (i < 6 ? 1.0 : 2.0);
            endPositive = LumaAt(uvPositive) - lumaLocalAverage;
            donePositive = abs(endPositive) >= gradientScaled;
        }
    }

    const float distanceNegative =
        horizontal ? (uv.x - uvNegative.x) : (uv.y - uvNegative.y);
    const float distancePositive =
        horizontal ? (uvPositive.x - uv.x) : (uvPositive.y - uv.y);
    const bool negativeCloser = distanceNegative < distancePositive;
    const float distanceNearest = min(distanceNegative, distancePositive);
    const float spanLength = distanceNegative + distancePositive;

    const bool centerSmaller = lumaM < lumaLocalAverage;
    const bool correctVariation =
        ((negativeCloser ? endNegative : endPositive) < 0.0) != centerSmaller;
    float pixelOffset = correctVariation
        ? (-distanceNearest / max(spanLength, 1.0e-5) + 0.5)
        : 0.0;
    pixelOffset = max(pixelOffset, subpixel);

    float2 finalUv = uv;
    if (horizontal)
    {
        finalUv.y += pixelOffset * stepLength;
    }
    else
    {
        finalUv.x += pixelOffset * stepLength;
    }

    const float4 result = g_source.SampleLevel(g_sourceSampler, finalUv, 0);
    g_destination[id.xy] = float4(result.rgb, centerColor.a);
}
)";

// Temporal resolve: exact reprojection from the camera pair (+ depth), 3x3
// neighbourhood variance clipping of the history, motion-adaptive feedback and
// a light sharpen. Sky pixels (no depth) reproject by direction only.
constexpr const char* kTaaShader = R"(
[[vk::binding(0, 0)]] RWTexture2D<float4> g_destination : register(u0);

[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] Texture2D g_color;
[[vk::binding(1, 0)]] [[vk::combinedImageSampler]] SamplerState g_colorSampler;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] Texture2D g_history;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] SamplerState g_historySampler;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] Texture2D g_depth;
[[vk::binding(3, 0)]] [[vk::combinedImageSampler]] SamplerState g_depthSampler;

struct Constants
{
    uint width;
    uint height;
    uint historyValid;
    uint pad;

    float4 forwardAspect;      // current forward, aspect
    float4 upTanHalfFov;       // current up, tan(fov / 2)
    float4 prevForwardTan;     // previous forward, previous tan(fov / 2)
    float4 prevUpFeedback;     // previous up, feedback
    float4 deltaNear;          // current camera - previous camera, near plane
    float4 farSharpen;         // x far plane, y sharpen
};
[[vk::push_constant]] Constants g;

float3 Compress(float3 c)
{
    c = max(c, 0.0);
    return c / (1.0 + max(c.r, max(c.g, c.b)));
}

float3 Uncompress(float3 c)
{
    return c / max(1.0 - max(c.r, max(c.g, c.b)), 1.0e-4);
}

float ViewDepth(float depth)
{
    const float nearPlane = max(g.deltaNear.w, 1.0e-5);
    const float farPlane = max(g.farSharpen.x, nearPlane + 1.0e-4);
    return nearPlane * farPlane /
        max(depth * (farPlane - nearPlane) + nearPlane, 1.0e-6);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g.width || id.y >= g.height)
    {
        return;
    }

    const int2 size = int2(g.width, g.height);
    const int2 pixel = int2(id.xy);
    const float2 uv = (float2(pixel) + 0.5) / float2(size);

    // Current-frame 3x3 neighbourhood in compressed space.
    float3 mean = 0.0;
    float3 square = 0.0;
    float3 lo = 1.0e9;
    float3 hi = -1.0e9;
    float3 center = 0.0;
    float alpha = 1.0;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 p = clamp(pixel + int2(dx, dy), int2(0, 0), size - 1);
            const float4 sampleColor = g_color.Load(int3(p, 0));
            const float3 c = Compress(sampleColor.rgb);
            mean += c;
            square += c * c;
            lo = min(lo, c);
            hi = max(hi, c);
            if (dx == 0 && dy == 0)
            {
                center = c;
                alpha = sampleColor.a;
            }
        }
    }
    mean /= 9.0;
    const float3 sigma = sqrt(max(square / 9.0 - mean * mean, 0.0));

    if (g.historyValid == 0u)
    {
        g_destination[id.xy] = float4(Uncompress(center), alpha);
        return;
    }

    // Reproject this pixel into the previous camera.
    const float3 forward = normalize(g.forwardAspect.xyz);
    const float3 upCurrent = normalize(g.upTanHalfFov.xyz);
    const float3 right = normalize(cross(forward, upCurrent));
    const float3 up = cross(right, forward);
    const float aspect = max(g.forwardAspect.w, 0.001);
    const float tanHalf = max(g.upTanHalfFov.w, 0.001);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float3 ray = normalize(
        forward + right * (ndc.x * aspect * tanHalf) + up * (ndc.y * tanHalf));

    const float depth = g_depth.SampleLevel(g_depthSampler, uv, 0).r;
    float3 previousRelative = ray;
    if (depth > 0.0)
    {
        previousRelative =
            ray * (ViewDepth(depth) / max(dot(ray, forward), 1.0e-5)) +
            g.deltaNear.xyz;
    }

    const float3 forwardPrev = normalize(g.prevForwardTan.xyz);
    const float3 upPrevRequested = normalize(g.prevUpFeedback.xyz);
    const float3 rightPrev = normalize(cross(forwardPrev, upPrevRequested));
    const float3 upPrev = cross(rightPrev, forwardPrev);
    const float tanPrev = max(g.prevForwardTan.w, 0.001);
    const float zPrev = dot(previousRelative, forwardPrev);

    float3 result = center;

    if (zPrev > 1.0e-4)
    {
        const float2 ndcPrev = float2(
            dot(previousRelative, rightPrev) / (zPrev * aspect * tanPrev),
            dot(previousRelative, upPrev) / (zPrev * tanPrev));
        const float2 uvPrev = float2(ndcPrev.x * 0.5 + 0.5, 0.5 - ndcPrev.y * 0.5);

        if (all(uvPrev > 0.0) && all(uvPrev < 1.0))
        {
            float3 history =
                Compress(g_history.SampleLevel(g_historySampler, uvPrev, 0).rgb);

            // Clip toward the neighbourhood's colour box. A plain min/max box
            // (with a little margin) is stable across jitter positions: a
            // tighter variance box changes with every sub-pixel offset at an
            // edge and drags the history after the current jittered sample.
            const float3 margin = 0.1 * (hi - lo) + 0.5 * sigma;
            const float3 boxMin = lo - margin;
            const float3 boxMax = hi + margin;
            const float3 boxCenter = 0.5 * (boxMin + boxMax);
            const float3 boxExtent = max(0.5 * (boxMax - boxMin), 1.0e-5);
            const float3 offset = history - boxCenter;
            const float3 scaled = abs(offset / boxExtent);
            const float outside = max(scaled.x, max(scaled.y, scaled.z));
            if (outside > 1.0)
            {
                history = boxCenter + offset / outside;
            }

            // Less history while things move on screen.
            const float2 motionPixels = (uvPrev - uv) * float2(size);
            const float feedback =
                clamp(g.prevUpFeedback.w, 0.0, 0.97) *
                saturate(1.0 - 0.06 * length(motionPixels));
            result = lerp(center, history, feedback);
        }
    }

    // Counter the resolve's softening with a gentle unsharp mask.
    result += (center - mean) * g.farSharpen.y;

    g_destination[id.xy] =
        float4(Uncompress(max(result, 0.0)), alpha);
}
)";

[[nodiscard]] u32 Bits(const f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}

[[nodiscard]] f32 Halton(u32 index, const u32 base) noexcept
{
    f32 result = 0.0F;
    f32 fraction = 1.0F;
    while (index > 0U)
    {
        fraction /= static_cast<f32>(base);
        result += fraction * static_cast<f32>(index % base);
        index /= base;
    }
    return result;
}

[[nodiscard]] math::Float3 SafeNormalize(const math::Float3& v) noexcept
{
    const f32 length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(length > 1.0e-20F) || !std::isfinite(length))
    {
        return {0.0F, 0.0F, 1.0F};
    }
    return {v.x / length, v.y / length, v.z / length};
}

[[nodiscard]] f32 Dot3(const math::Float3& a, const math::Float3& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
} // namespace

std::string_view AntiAliasingModeName(const AntiAliasingMode mode) noexcept
{
    switch (mode)
    {
    case AntiAliasingMode::Off:
        return "off";
    case AntiAliasingMode::Fxaa:
        return "fxaa";
    case AntiAliasingMode::Taa:
        return "taa";
    }
    return "off";
}

bool ParseAntiAliasingMode(
    const std::string_view text,
    AntiAliasingMode& mode) noexcept
{
    std::string lower;
    for (const char c : text)
    {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }

    if (lower == "off" || lower == "none")
    {
        mode = AntiAliasingMode::Off;
    }
    else if (lower == "fxaa")
    {
        mode = AntiAliasingMode::Fxaa;
    }
    else if (lower == "taa")
    {
        mode = AntiAliasingMode::Taa;
    }
    else
    {
        return false;
    }
    return true;
}

std::array<f32, 2> TaaJitterPixels(const u32 frameCounter) noexcept
{
    // Index 0 is skipped: Halton(0) is the pixel centre.
    const u32 index = frameCounter % 8U + 1U;
    return {Halton(index, 2U) - 0.5F, Halton(index, 3U) - 0.5F};
}

void ApplyCameraJitter(
    const math::Float3& forward,
    const math::Float3& up,
    const f32 verticalFovRadians,
    const u32 height,
    const std::array<f32, 2>& jitterPixels,
    math::Float3& jitteredForward,
    math::Float3& jitteredUp) noexcept
{
    const math::Float3 f = SafeNormalize(forward);
    math::Float3 right = {
        f.y * up.z - f.z * up.y,
        f.z * up.x - f.x * up.z,
        f.x * up.y - f.y * up.x};
    right = SafeNormalize(right);
    const math::Float3 u = {
        right.y * f.z - right.z * f.y,
        right.z * f.x - right.x * f.z,
        right.x * f.y - right.y * f.x};

    // One pixel at the image centre, as a tangent.
    const f32 pixelTangent =
        2.0F * std::tan(std::max(verticalFovRadians, 1.0e-4F) * 0.5F) /
        static_cast<f32>(std::max(height, 1U));
    const f32 dx = jitterPixels[0] * pixelTangent;
    const f32 dy = jitterPixels[1] * pixelTangent;

    jitteredForward = SafeNormalize({
        f.x + right.x * dx + u.x * dy,
        f.y + right.y * dx + u.y * dy,
        f.z + right.z * dx + u.z * dy});

    const f32 along = Dot3(u, jitteredForward);
    jitteredUp = SafeNormalize({
        u.x - jitteredForward.x * along,
        u.y - jitteredForward.y * along,
        u.z - jitteredForward.z * along});
}

bool TaaHistoryUsable(
    const TaaCamera& previous,
    const TaaCamera& current) noexcept
{
    const f64 jump = math::Length(current.positionMeters - previous.positionMeters);
    if (!std::isfinite(jump) || jump > 2000.0)
    {
        return false;
    }

    const f32 forwardDot = Dot3(SafeNormalize(previous.forward), SafeNormalize(current.forward));
    if (forwardDot < 0.8F)
    {
        return false;
    }

    return std::abs(previous.verticalFovRadians - current.verticalFovRadians) <
               1.0e-3F &&
           std::abs(previous.nearPlaneMeters - current.nearPlaneMeters) <
               1.0e-6F &&
           std::abs(previous.farPlaneMeters - current.farPlaneMeters) <
               1.0e-1F * std::max(current.farPlaneMeters, 1.0F);
}

AntiAliasingRenderer::AntiAliasingRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto fxaa = compiler.Compile({
        .source = kFxaaShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false});
    fxaaPipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = fxaa.bytecode.data(),
            .size = fxaa.bytecode.size()},
        .pushConstantDwords = kFxaaPushDwords,
        .shaderResourceBuffers = 0U,
        .storageTextures = 1U,
        .sampledTextures = 1U});

    const auto taa = compiler.Compile({
        .source = kTaaShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false});
    taaPipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = taa.bytecode.data(),
            .size = taa.bytecode.size()},
        .pushConstantDwords = kTaaPushDwords,
        .shaderResourceBuffers = 0U,
        .storageTextures = 1U,
        .sampledTextures = 3U});
}

void AntiAliasingRenderer::Fxaa(
    rhi::CommandList& commands,
    rhi::Texture& source,
    rhi::Texture& destination,
    const u32 width,
    const u32 height)
{
    if (fxaaPipeline_ == nullptr || width == 0U || height == 0U)
    {
        return;
    }

    const std::array<u32, kFxaaPushDwords> constants{width, height, 0U, 0U};
    commands.SetComputePipeline(*fxaaPipeline_);
    commands.SetComputeConstants(constants);
    commands.SetComputeStorageTexture(0U, destination);
    commands.SetComputeTexture(0U, source);
    commands.Dispatch((width + 7U) / 8U, (height + 7U) / 8U, 1U);
}

void AntiAliasingRenderer::Taa(
    rhi::CommandList& commands,
    rhi::Texture& color,
    rhi::Texture& depth,
    rhi::Texture& history,
    rhi::Texture& destination,
    const u32 width,
    const u32 height,
    const TaaCamera& current,
    const TaaCamera& previous,
    const bool historyValid,
    const TaaSettings& settings)
{
    if (taaPipeline_ == nullptr || width == 0U || height == 0U)
    {
        return;
    }

    const f32 aspect =
        static_cast<f32>(width) / static_cast<f32>(height);
    const math::Double3 delta =
        current.positionMeters - previous.positionMeters;

    const std::array<u32, kTaaPushDwords> constants{
        width, height, historyValid ? 1U : 0U, 0U,

        Bits(current.forward.x), Bits(current.forward.y),
        Bits(current.forward.z), Bits(aspect),

        Bits(current.up.x), Bits(current.up.y), Bits(current.up.z),
        Bits(std::tan(current.verticalFovRadians * 0.5F)),

        Bits(previous.forward.x), Bits(previous.forward.y),
        Bits(previous.forward.z),
        Bits(std::tan(previous.verticalFovRadians * 0.5F)),

        Bits(previous.up.x), Bits(previous.up.y), Bits(previous.up.z),
        Bits(settings.feedback),

        Bits(static_cast<f32>(delta.x)), Bits(static_cast<f32>(delta.y)),
        Bits(static_cast<f32>(delta.z)),
        Bits(std::max(current.nearPlaneMeters, 1.0e-5F)),

        Bits(std::max(
            current.farPlaneMeters,
            current.nearPlaneMeters + 1.0e-4F)),
        Bits(settings.sharpen), 0U, 0U};

    commands.SetComputePipeline(*taaPipeline_);
    commands.SetComputeConstants(constants);
    commands.SetComputeStorageTexture(0U, destination);
    commands.SetComputeTexture(0U, color);
    commands.SetComputeTexture(1U, history);
    commands.SetComputeTexture(2U, depth);
    commands.Dispatch((width + 7U) / 8U, (height + 7U) / 8U, 1U);
}
} // namespace orbit::post_process
