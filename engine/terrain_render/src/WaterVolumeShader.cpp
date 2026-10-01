#include <orbit/terrain_render/WaterVolumeShader.hpp>

#include <stdexcept>
#include <string>

namespace orbit::terrain_render
{
namespace
{
[[nodiscard]] std::size_t Require(
    const std::string& text,
    const std::string_view marker,
    const std::size_t from,
    const char* what)
{
    const auto found = text.find(marker, from);
    if (found == std::string::npos)
    {
        throw std::runtime_error(
            std::string("Orbit water pass could not locate ") + what + ".");
    }
    return found;
}

void ReplaceOnce(
    std::string& text,
    const std::string_view marker,
    const std::string_view replacement,
    const char* what)
{
    const auto at = Require(text, marker, 0, what);
    text.replace(at, marker.size(), replacement);
}

constexpr std::string_view kBedWaterReplacement =
    R"(    // Standing water is a separate object, drawn by the near-field water pass.
    const float waterCoverage = 0.0;
    float3 surfaceNormal = bodyFixedNormal;

)";

constexpr std::string_view kWaterVertexElevation =
    R"(    const float bedElevation = asfloat(g_samples.Load(sampleByteOffset));
    const float waterDepth = asfloat(g_samples.Load(sampleByteOffset + 20u));
    // A triangle is drawn only if one of its corners is wet. The flat plane over
    // its dry corners lies under the terrain, which the pixel stage rejects
    // against the terrain depth: that intersection is the shoreline.
    const bool secondTriangle = cornerIndex >= 3u;
    float deepestCorner = 0.0;
    [unroll]
    for (uint corner = 0u; corner < 3u; ++corner)
    {
        const uint cornerSampleX = cellX +
            ((corner == 0u) ? (secondTriangle ? 1u : 0u)
                            : (corner == 1u ? 0u : 1u));
        const uint cornerSampleY = cellY +
            ((corner == 0u) ? 0u
                            : (corner == 1u ? 1u : (secondTriangle ? 1u : 0u)));
        const uint cornerPhysicalIndex = PhysicalSampleIndex(
            cornerSampleX, cornerSampleY, resolution, originX, originY);
        deepestCorner = max(
            deepestCorner,
            asfloat(g_samples.Load(cornerPhysicalIndex * 32u + 20u)));
    }
    // The water surface: the sea plane, or bed + depth where a lake stands.
    const float seaLevel = g_pc.g_centerOffsetMeters.w;
    const float elevation =
        waterDepth > 0.0 ? bedElevation + waterDepth : seaLevel;)";

constexpr std::string_view kWaterPixelBody = R"(
struct WaterOptics
{
    // xyz absorption per metre (1/m), w refractive index.
    float4 absorptionAndIndex;
    // xyz deep-water albedo, w depth (m) at which the column reads as deep.
    float4 deepColorAndDepth;
    // x surface roughness, y opacity (near-field representation weight),
    // z planet radius (m).
    float4 roughnessOpacityRadius;
    // x near plane, y far plane (m) of the reverse-Z projection.
    float4 projection;
    // xyz direction to the sun (body-fixed), w its irradiance.
    float4 sunAndIrradiance;
    // xyz sky irradiance (linear).
    float4 skyIrradiance;
};

[[vk::binding(1, 0)]]
StructuredBuffer<WaterOptics> g_waterOptics : register(t1);

[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] Texture2D g_terrainDepth;
[[vk::binding(2, 0)]] [[vk::combinedImageSampler]] SamplerState g_terrainDepthSampler;

float RippleHash(float3 p)
{
    p = frac(p * 0.3183099 + float3(0.1, 0.2, 0.3));
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float RippleNoise(float3 p)
{
    const float3 cell = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    const float n000 = RippleHash(cell + float3(0.0, 0.0, 0.0));
    const float n100 = RippleHash(cell + float3(1.0, 0.0, 0.0));
    const float n010 = RippleHash(cell + float3(0.0, 1.0, 0.0));
    const float n110 = RippleHash(cell + float3(1.0, 1.0, 0.0));
    const float n001 = RippleHash(cell + float3(0.0, 0.0, 1.0));
    const float n101 = RippleHash(cell + float3(1.0, 0.0, 1.0));
    const float n011 = RippleHash(cell + float3(0.0, 1.0, 1.0));
    const float n111 = RippleHash(cell + float3(1.0, 1.0, 1.0));
    return lerp(
        lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
        lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y),
        f.z);
}

float DistributionGgx(float nDotH, float roughness)
{
    const float a = max(roughness * roughness, 0.0025);
    const float a2 = a * a;
    const float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1.0e-5);
}

float GeometrySchlickGgx(float nDotV, float roughness)
{
    const float r = roughness + 1.0;
    const float k = (r * r) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 1.0e-5);
}

float4 main(VSOutput input) : SV_Target0
{
    {
        // Same per-pixel level ownership as the terrain pass (see the vertex
        // shader's lodFade), so overlapping levels draw their water once.
        const float ditherNoise = frac(52.9829189 *
            frac(dot(input.position.xy, float2(0.06711056, 0.00583715))));
        if (ditherNoise >= input.lodFade.x || ditherNoise < input.lodFade.y)
            discard;
    }
    const WaterOptics water = g_waterOptics[0];
    const float nearPlane = max(water.projection.x, 1.0e-3);
    const float farPlane = max(water.projection.y, nearPlane * 2.0);

    // Derivatives before any divergent control flow.
    const float pixelFootprint =
        max(length(fwidth(input.localPosition)), 1.0e-4);

    // The terrain in front of or under this water fragment. A fragment behind
    // the terrain is land over the water plane: that is the shoreline.
    const float terrainDepth =
        g_terrainDepth.Load(int3(int2(input.position.xy), 0)).r;
    const float terrainViewDepth =
        terrainDepth > 0.0
            ? nearPlane * farPlane /
                max(terrainDepth * (farPlane - nearPlane) + nearPlane, 1.0e-6)
            : farPlane;
    const float surfaceViewDepth = 1.0 / max(input.position.w, 1.0e-9);
    const float behind = terrainViewDepth - surfaceViewDepth;
    if (behind <= 0.0)
    {
        discard;
    }

    const float3 waterUp = normalize(input.bodyFixedSurfaceDirection);
    const float3 viewVector = normalize(-input.bodyFixedRay);

    // World-fixed ripples (there is no time input). Each octave fades out once
    // its wavelength nears the pixel footprint.
    const float3 wavePosition = waterUp * max(water.roughnessOpacityRadius.z, 1.0);
    const float3 arbitrary =
        abs(waterUp.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 waveTangent = normalize(cross(arbitrary, waterUp));
    const float3 waveBitangent = cross(waterUp, waveTangent);
    float2 rippleSlope = float2(0.0, 0.0);
    [unroll]
    for (uint octave = 0u; octave < 2u; ++octave)
    {
        const float wavelength = octave == 0u ? 14.0 : 5.0;
        const float fade = smoothstep(2.0, 6.0, wavelength / pixelFootprint);
        const float3 p = wavePosition / wavelength;
        const float h0 = RippleNoise(p);
        const float hT = RippleNoise(p + waveTangent * 0.3);
        const float hB = RippleNoise(p + waveBitangent * 0.3);
        rippleSlope += float2(hT - h0, hB - h0) * (0.11 * fade);
    }
    const float3 normal = normalize(
        waterUp - waveTangent * rippleSlope.x - waveBitangent * rippleSlope.y);

    const float refractiveIndex = max(water.absorptionAndIndex.w, 1.0);
    const float cosView = saturate(dot(normal, viewVector));
    const float cosUp = saturate(dot(waterUp, viewVector));

    // Water column under this pixel: the straight ray from the surface to where
    // the terrain pass drew the bed, turned into a vertical depth and then a
    // refracted in-water path. The optical path is deliberately short (0.6x) so
    // terrain stays readable through water tens of metres deep.
    const float rayMeters =
        behind * length(input.localPosition) / max(surfaceViewDepth, 1.0e-3);
    const float verticalDepth = rayMeters * cosUp;
    const float sinRefracted =
        sqrt(max(1.0 - cosUp * cosUp, 0.0)) / refractiveIndex;
    const float cosRefracted =
        sqrt(max(1.0 - sinRefracted * sinRefracted, 0.0));
    const float pathMeters = 0.6 * verticalDepth / max(cosRefracted, 0.08);

    const float3 absorption = max(water.absorptionAndIndex.xyz, 0.0);
    // Alpha blending transmits the terrain by a single factor. Red is gone within
    // a few metres, so weighting it in would make every column opaque; use the
    // green and blue absorption, which is what survives to depth. The colour cast
    // comes from the body colour.
    const float absorptionRate = dot(absorption, float3(0.0, 0.5, 0.5));
    const float transmittance = exp(-absorptionRate * pathMeters);

    const float3 deepAlbedo = max(water.deepColorAndDepth.xyz, 0.0);
    const float deepDepth = max(water.deepColorAndDepth.w, 0.5);
    const float bodyWeight = 1.0 - exp(-verticalDepth / (2.0 * deepDepth));

    const float3 sunDirection = normalize(water.sunAndIrradiance.xyz);
    const float sunIrradiance = max(water.sunAndIrradiance.w, 0.0);
    const float3 skyIrradiance = max(water.skyIrradiance.xyz, 0.0);
    const float3 sunLit =
        sunIrradiance * saturate(dot(waterUp, sunDirection));

    // In-scattered body colour lit by the sun and the sky.
    const float3 bodyRadiance =
        deepAlbedo * (sunLit + skyIrradiance) / 3.14159265;

    // Fresnel reflection of the sky and the sun glint.
    const float f0 = pow((refractiveIndex - 1.0) / (refractiveIndex + 1.0), 2.0);
    const float fresnel = f0 + (1.0 - f0) * pow(1.0 - cosView, 5.0);
    const float3 skyRadiance =
        skyIrradiance / 3.14159265 * (0.55 + 0.45 * (1.0 - cosView));

    const float roughness = clamp(water.roughnessOpacityRadius.x, 0.02, 0.6);
    const float nDotL = saturate(dot(normal, sunDirection));
    const float3 halfVector = normalize(sunDirection + viewVector);
    const float nDotH = saturate(dot(normal, halfVector));
    const float vDotH = saturate(dot(viewVector, halfVector));
    const float glintFresnel = f0 + (1.0 - f0) * pow(1.0 - vDotH, 5.0);
    const float glint =
        DistributionGgx(nDotH, roughness) *
        GeometrySchlickGgx(cosView, roughness) *
        GeometrySchlickGgx(nDotL, roughness) * glintFresnel /
        max(4.0 * cosView * nDotL, 1.0e-4) * nDotL * sunIrradiance;

    // out = terrain * (1 - alpha) + colour * alpha, with
    //   alpha  = 1 - (1 - fresnel) * transmittance
    //   colour * alpha = reflection + (1 - fresnel) * (1 - transmittance) * body
    const float alpha =
        max(1.0 - (1.0 - fresnel) * transmittance, 0.02);
    const float3 premultiplied =
        fresnel * skyRadiance +
        (1.0 - fresnel) * (1.0 - transmittance) *
            lerp(bodyRadiance * 0.5, bodyRadiance, bodyWeight) +
        glint;

    const float opacity = saturate(water.roughnessOpacityRadius.y);
    // RGBA16F holds up to 65504; a mirror-aligned glint over a tiny alpha must
    // not overflow it.
    const float3 colour = min(premultiplied / alpha, 60000.0);
    return float4(colour, alpha * opacity);
}
)";
} // namespace

std::string BuildClipmapBedPixelShader(const std::string_view baseShader)
{
    std::string result(baseShader);

    // Level cross-fade: a screen-door dither between a level fading in or out
    // and the level around it, so a plan change dissolves instead of popping.
    ReplaceOnce(
        result,
        "    float drySurface : TEXCOORD11;\n    float horizonClip : SV_ClipDistance0;",
        "    float drySurface : TEXCOORD11;\n"
        "    float2 lodFade : TEXCOORD13;\n"
        "    float horizonClip : SV_ClipDistance0;",
        "the terrain pixel input struct");
    ReplaceOnce(
        result,
        "SurfaceOutputs main(VSOutput input)\n{\n",
        "SurfaceOutputs main(VSOutput input)\n{\n"
        "    {\n"
        "        // Interleaved gradient noise: stable per pixel, evenly spread.\n"
        "        const float ditherNoise = frac(52.9829189 *\n"
        "            frac(dot(input.position.xy, float2(0.06711056, 0.00583715))));\n"
        "        if (ditherNoise >= input.lodFade.x || ditherNoise < input.lodFade.y)\n"
        "            discard;\n"
        "    }\n",
        "the terrain pixel entry point");

    ReplaceOnce(
        result,
        "const float3 oceanColor =\n        float3(\n            0.025,\n            0.11,\n            0.24);",
        "// Silt: what lies under the water. The water pass draws the water.\n"
        "    const float3 oceanColor =\n        float3(\n            0.30,\n            0.27,\n            0.20);",
        "the ocean biome colour");

    constexpr std::string_view kStart =
        "    // Depth gives a continuous shallow shoreline without a lifted overlay.";
    constexpr std::string_view kEnd = "    SurfaceOutputs output;\n";
    const auto start = Require(result, kStart, 0, "the standing-water block");
    const auto end = Require(result, kEnd, start, "the end of the standing-water block");
    result.replace(start, end - start, kBedWaterReplacement);
    return result;
}

std::string BuildClipmapWaterVertexShader(
    const std::string_view terrainVertexShader)
{
    std::string result(terrainVertexShader);

    // 1. Vertices on the water surface, and the triangle's wet test.
    const std::string_view elevationStart =
        "    const float bedElevation = asfloat(g_samples.Load(sampleByteOffset));";
    const std::string_view elevationEnd = "    const float elevation = bedElevation;";
    const auto start = Require(result, elevationStart, 0, "the vertex elevation block");
    const auto end =
        Require(result, elevationEnd, start, "the vertex elevation assignment") +
        elevationEnd.size();
    result.replace(start, end - start, kWaterVertexElevation);

    // 2. Outputs: the water depth at this vertex and the view ray in the
    //    body-fixed frame (for view-dependent shading).
    ReplaceOnce(
        result,
        "    output.elevation = elevation;\n    output.waterDepth = 0.0;",
        "    output.elevation = elevation;\n"
        "    output.waterDepth = waterDepth;\n"
        "    output.bodyFixedRay =\n"
        "        g_pc.g_observerEastBody.xyz * localPosition.x +\n"
        "        g_pc.g_observerUpBody.xyz * localPosition.y +\n"
        "        g_pc.g_observerNorthBody.xyz * localPosition.z;",
        "the vertex outputs");

    ReplaceOnce(
        result,
        "    float horizonClip : SV_ClipDistance0;\n};",
        "    float3 bodyFixedRay : TEXCOORD12;\n    float horizonClip : SV_ClipDistance0;\n};",
        "the vertex output struct");

    // 3. Dry triangles are not drawn.
    const auto returnAt = result.rfind("    return output;\n}");
    if (returnAt == std::string::npos)
    {
        throw std::runtime_error(
            "Orbit water pass could not locate the vertex shader return.");
    }
    result.insert(
        returnAt,
        "    if (deepestCorner <= 0.0)\n"
        "    {\n"
        "        output.horizonClip = -1.0;\n"
        "    }\n");
    return result;
}

std::string BuildClipmapWaterPixelShader(
    const std::string_view waterVertexShader)
{
    const std::string vertex(waterVertexShader);
    const auto structStart = Require(vertex, "struct VSOutput\n{", 0, "the VSOutput struct");
    const auto structEnd = Require(vertex, "\n};\n", structStart, "the end of VSOutput") + 4;

    std::string result = vertex.substr(structStart, structEnd - structStart);
    result += kWaterPixelBody;
    return result;
}
} // namespace orbit::terrain_render
