#include <orbit/terrain_render/SurfaceEffectShader.hpp>

#include <stdexcept>
#include <string>

namespace orbit::terrain_render
{
namespace
{
void ReplaceOnce(
    std::string& target,
    const std::string_view needle,
    const std::string_view replacement)
{
    const auto position = target.find(needle);
    if (position == std::string::npos)
    {
        throw std::runtime_error(
            "Orbit M38 could not locate terrain shader injection marker.");
    }

    target.replace(
        position,
        needle.size(),
        replacement);
}

constexpr std::string_view kDeclarations = R"(
struct SurfaceEffectStamp
{
    float3 bodyFixedDirection;
    float angularRadiusRadians;
    float amount;
    uint effect;
    float2 reserved;
};

[[vk::binding(1, 0)]]
StructuredBuffer<SurfaceEffectStamp> g_surfaceEffects : register(t1);

struct SurfaceEffectInfluence
{
    float wetness;
    float soot;
    float ash;
    float sediment;
    float heat;
};

SurfaceEffectInfluence EvaluateSurfaceEffects(float3 bodyFixedDirection)
{
    SurfaceEffectInfluence result = (SurfaceEffectInfluence)0;
    const float3 direction = normalize(bodyFixedDirection);

    [loop]
    for (uint index = 0u; index < 512u; ++index)
    {
        const SurfaceEffectStamp stamp = g_surfaceEffects[index];

        // The CPU upload zero-fills unused entries, making this an explicit
        // sentinel and keeping the common low-count path cheap.
        if (stamp.angularRadiusRadians <= 0.0)
        {
            break;
        }

        const float cosine =
            clamp(
                dot(direction, normalize(stamp.bodyFixedDirection)),
                -1.0,
                1.0);
        const float distance = acos(cosine);

        if (distance >= stamp.angularRadiusRadians)
        {
            continue;
        }

        const float normalized =
            1.0 - distance / max(stamp.angularRadiusRadians, 1.0e-9);
        const float influence =
            max(stamp.amount, 0.0) * normalized * normalized;

        if (stamp.effect == 0u) result.wetness += influence;
        else if (stamp.effect == 1u) result.soot += influence;
        else if (stamp.effect == 2u) result.ash += influence;
        else if (stamp.effect == 3u) result.sediment += influence;
        else if (stamp.effect == 4u) result.heat += influence;
    }

    result.wetness = saturate(result.wetness);
    result.soot = saturate(result.soot);
    result.ash = saturate(result.ash);
    result.sediment = saturate(result.sediment);
    result.heat = max(result.heat, 0.0);
    return result;
}

)";

constexpr std::string_view kMaterialStage = R"(
    float3 surfaceEmission = float3(0.0, 0.0, 0.0);

    // Deposits affect the physical ground, not the overlying standing-water
    // layer. Shoreline pixels blend the effect out continuously with coverage.
    SurfaceEffectInfluence effects =
        EvaluateSurfaceEffects(input.bodyFixedSurfaceDirection);
    const float exposedGround = 1.0 - waterCoverage;
    effects.wetness *= exposedGround;
    effects.soot *= exposedGround;
    effects.ash *= exposedGround;
    effects.sediment *= exposedGround;
    effects.heat *= exposedGround;

    const float effectLighting =
        lighting *
        (0.92 + elevationLight * 0.15);

    if (effects.wetness > 0.0)
    {
        surfaceBaseColor =
            lerp(
                surfaceBaseColor,
                surfaceBaseColor * 0.52,
                effects.wetness);
        color =
            lerp(
                color,
                color * 0.52,
                effects.wetness);
        surfaceRoughness =
            lerp(surfaceRoughness, 0.12, effects.wetness);
    }

    if (effects.soot > 0.0)
    {
        const float3 sootColor = float3(0.018, 0.016, 0.014);
        surfaceBaseColor = lerp(surfaceBaseColor, sootColor, effects.soot);
        color = lerp(color, sootColor * effectLighting, effects.soot);
        surfaceRoughness = lerp(surfaceRoughness, 0.94, effects.soot);
        surfaceMetallic *= 1.0 - effects.soot;
    }

    if (effects.ash > 0.0)
    {
        const float3 ashColor = float3(0.43, 0.42, 0.40);
        surfaceBaseColor = lerp(surfaceBaseColor, ashColor, effects.ash);
        color = lerp(color, ashColor * effectLighting, effects.ash);
        surfaceRoughness = lerp(surfaceRoughness, 0.98, effects.ash);
        surfaceMetallic *= 1.0 - effects.ash;
    }

    if (effects.sediment > 0.0)
    {
        const float3 sedimentColor = float3(0.36, 0.23, 0.11);
        surfaceBaseColor =
            lerp(surfaceBaseColor, sedimentColor, effects.sediment);
        color =
            lerp(color, sedimentColor * effectLighting, effects.sediment);
        surfaceRoughness =
            lerp(surfaceRoughness, 0.90, effects.sediment);
    }

    if (effects.heat > 0.0)
    {
        const float hot = min(effects.heat, 8.0);
        surfaceEmission += float3(
            3.5 * hot,
            0.72 * hot * min(hot, 2.0),
            0.08 * hot * max(hot - 0.35, 0.0));
        surfaceRoughness =
            saturate(surfaceRoughness + 0.08 * saturate(effects.heat));
        color += surfaceEmission * 0.15;
    }

    SurfaceOutputs output;
)";
} // namespace

std::string BuildSurfaceEffectPixelShader(
    const std::string_view baseShader)
{
    std::string result(baseShader);

    ReplaceOnce(
        result,
        "struct VSOutput\n",
        std::string(kDeclarations) + "struct VSOutput\n");

    ReplaceOnce(
        result,
        "    SurfaceOutputs output;\n",
        kMaterialStage);

    ReplaceOnce(
        result,
        "            0.0,\n            0.0,\n            0.0,\n            EncodeSurfaceMeta(surfaceClass, 1.0));",
        "            surfaceEmission.x,\n            surfaceEmission.y,\n            surfaceEmission.z,\n            EncodeSurfaceMeta(surfaceClass, 1.0));");

    return result;
}
} // namespace orbit::terrain_render
