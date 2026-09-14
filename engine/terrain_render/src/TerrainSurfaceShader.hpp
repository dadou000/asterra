#pragma once
namespace orbit::terrain_render::detail
{
inline constexpr const char* kTerrainSurfacePixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float elevation : TEXCOORD0;
    float4 biome0 : TEXCOORD1;
    float4 biome1 : TEXCOORD2;
    float3 terrainNormal : TEXCOORD3;
    float3 surfaceDirection : TEXCOORD4;
    float waterDepth : TEXCOORD5;
    float3 localPosition : TEXCOORD6;
    float horizonClip : SV_ClipDistance0;
};

float4 main(VSOutput input) : SV_Target0
{
    float4 biome0 =
        max(
            input.biome0,
            0.0);

    float4 biome1 =
        max(
            input.biome1,
            0.0);

    const float weightSum =
        biome0.x +
        biome0.y +
        biome0.z +
        biome0.w +
        biome1.x +
        biome1.y +
        biome1.z +
        biome1.w;

    const float inverseWeight =
        1.0 /
        max(
            weightSum,
            0.0001);

    biome0 *= inverseWeight;
    biome1 *= inverseWeight;

    const float3 oceanColor =
        float3(
            0.025,
            0.11,
            0.24);

    const float3 desertColor =
        float3(
            0.72,
            0.56,
            0.31);

    const float3 grasslandColor =
        float3(
            0.26,
            0.42,
            0.16);

    const float3 temperateForestColor =
        float3(
            0.075,
            0.25,
            0.11);

    const float3 borealForestColor =
        float3(
            0.08,
            0.20,
            0.16);

    const float3 tundraColor =
        float3(
            0.43,
            0.48,
            0.42);

    const float3 alpineColor =
        float3(
            0.58,
            0.59,
            0.57);

    const float3 wetlandColor =
        float3(
            0.09,
            0.27,
            0.22);

    float3 color =
        oceanColor *
            biome0.x +
        desertColor *
            biome0.y +
        grasslandColor *
            biome0.z +
        temperateForestColor *
            biome0.w +
        borealForestColor *
            biome1.x +
        tundraColor *
            biome1.y +
        alpineColor *
            biome1.z +
        wetlandColor *
            biome1.w;

    const float3 terrainNormal =
        normalize(
            input.terrainNormal);

    const float3 surfaceDirection =
        normalize(
            input.surfaceDirection);

    const float slopeCosine =
        saturate(
            dot(
                terrainNormal,
                surfaceDirection));

    const float slopeStrength =
        1.0 -
        slopeCosine;

    const float landWeight =
        saturate(
            1.0 -
            biome0.x);

    const float rockBlend =
        smoothstep(
            0.06,
            0.34,
            slopeStrength) *
        landWeight *
        0.72;

    const float3 rockColor =
        float3(
            0.30,
            0.295,
            0.285);

    color =
        lerp(
            color,
            rockColor,
            rockBlend);

    const float3 previewLightDirection =
        normalize(
            float3(
                -0.42,
                0.78,
                0.46));

    const float diffuse =
        saturate(
            dot(
                terrainNormal,
                previewLightDirection));

    const float hemispheric =
        0.58 +
        0.42 *
        slopeCosine;

    const float lighting =
        (0.36 +
         diffuse * 0.64) *
        hemispheric;

    const float elevationLight =
        saturate(
            input.elevation /
                8000.0);

    color *=
        lighting *
        (0.92 +
         elevationLight *
            0.15);

    // Depth gives a continuous shallow shoreline without a lifted overlay.
    // Use the sphere normal for standing water; bank slopes still shade land.
    const float waterCoverage = smoothstep(0.0, 0.25, input.waterDepth);
    if (waterCoverage > 0.0)
    {
        const float3 viewDirection = normalize(-input.localPosition);
        const float fresnel = pow(1.0 - saturate(dot(surfaceDirection, viewDirection)), 5.0);
        const float shallow = exp2(-max(input.waterDepth, 0.0) / 12.0);
        float3 waterColor = lerp(float3(0.012, 0.075, 0.13),
            float3(0.055, 0.23, 0.25), shallow);
        waterColor = lerp(waterColor, float3(0.16, 0.34, 0.42), fresnel * 0.72);
        color = lerp(color, waterColor, waterCoverage);
    }
    return float4(color, 1.0);
}
)";
}
