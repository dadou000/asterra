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
    float spacingMeters : TEXCOORD7;
    // Camera-independent (direction * planetRadius) -- see the
    // comment on this field where each vertex shader sets it. Used
    // instead of localPosition/surfaceDirection for anything that must
    // stay fixed for a given physical point as the camera moves.
    float3 worldPosition : TEXCOORD8;
    float horizonClip : SV_ClipDistance0;
};

// Fine surface detail (rock/ground micro-relief) is only ever baked
// into vertex positions/normals at a fine enough resolution to
// represent it geometrically -- see LoadFineSlope in
// TerrainPreviewRenderer.cpp, which is why that renderer always
// passes spacingMeters == 0 here (fully disabling this fake bump: its
// terrainNormal is already real ground truth, and layering synthetic
// noise on top of it would fight the real shape instead of matching
// it). UniformPlanetRenderer's whole-planet mesh has no equivalent
// per-vertex fine data, so it still uses this to fake the same-looking
// detail as a shading-only normal perturbation: it never moves a
// vertex, so it can't alias, and it fades out toward finer spacing
// (see detailFade below).
float DetailHash(float3 p)
{
    p = frac(p * 0.3183099 + float3(0.1, 0.2, 0.3));
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float DetailValueNoise(float3 p)
{
    const float3 cell = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);

    const float n000 = DetailHash(cell + float3(0.0, 0.0, 0.0));
    const float n100 = DetailHash(cell + float3(1.0, 0.0, 0.0));
    const float n010 = DetailHash(cell + float3(0.0, 1.0, 0.0));
    const float n110 = DetailHash(cell + float3(1.0, 1.0, 0.0));
    const float n001 = DetailHash(cell + float3(0.0, 0.0, 1.0));
    const float n101 = DetailHash(cell + float3(1.0, 0.0, 1.0));
    const float n011 = DetailHash(cell + float3(0.0, 1.0, 1.0));
    const float n111 = DetailHash(cell + float3(1.0, 1.0, 1.0));

    const float nx00 = lerp(n000, n100, f.x);
    const float nx10 = lerp(n010, n110, f.x);
    const float nx01 = lerp(n001, n101, f.x);
    const float nx11 = lerp(n011, n111, f.x);

    const float nxy0 = lerp(nx00, nx10, f.y);
    const float nxy1 = lerp(nx01, nx11, f.y);

    return lerp(nxy0, nxy1, f.z);
}

// Wavelength scales with this level's own sample spacing rather than
// a fixed size: a coarse level (rendered only from far away) gets
// proportionally broad bumps that read cleanly at that distance,
// while a finer level gets proportionally small ones -- so this
// always looks like "one LOD finer" of the same terrain instead of
// either aliasing into noise or vanishing below a pixel.
float DetailHeight(float3 worldPosition, float spacingMeters)
{
    float height = 0.0;
    height += (DetailValueNoise(worldPosition / (spacingMeters * 2.2)) - 0.5) * 1.0;
    height += (DetailValueNoise(worldPosition / (spacingMeters * 0.7)) - 0.5) * 0.4;
    return height;
}

// Perturbs a base normal with fine bump detail evaluated in world
// space (continuous across tiles/levels, so it never seams), fading
// out toward finer spacing where real geometry already resolves it.
float3 ApplyDetailNormal(
    float3 baseNormal,
    float3 worldPosition,
    float spacingMeters)
{
    const float fade =
        smoothstep(20.0, 260.0, spacingMeters);

    if (fade <= 0.0)
    {
        return baseNormal;
    }

    const float3 arbitrary =
        abs(baseNormal.y) < 0.99
            ? float3(0.0, 1.0, 0.0)
            : float3(1.0, 0.0, 0.0);

    const float3 tangent =
        normalize(cross(arbitrary, baseNormal));

    const float3 bitangent =
        cross(baseNormal, tangent);

    // A fraction of the noise's own wavelength, so the finite
    // difference always samples within one "bump" regardless of how
    // coarse this level is.
    const float epsilon = spacingMeters * 0.25;

    const float h0 = DetailHeight(worldPosition, spacingMeters);
    const float hT = DetailHeight(worldPosition + tangent * epsilon, spacingMeters);
    const float hB = DetailHeight(worldPosition + bitangent * epsilon, spacingMeters);

    const float dT = (hT - h0) / epsilon;
    const float dB = (hB - h0) / epsilon;

    // Apparent bump relief as a fraction of this level's own spacing
    // -- keeps the bump-to-wavelength ratio (and so how "steep" it
    // reads) consistent across levels instead of a fixed meter count
    // that would look sharp up close and flat far away (or the
    // reverse).
    const float bumpStrength = spacingMeters * 0.35;

    const float3 perturbed =
        normalize(
            baseNormal -
            tangent * dT * bumpStrength -
            bitangent * dB * bumpStrength);

    return normalize(lerp(baseNormal, perturbed, fade));
}

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
        ApplyDetailNormal(
            normalize(input.terrainNormal),
            input.worldPosition,
            input.spacingMeters);

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
