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
    float3 bodyFixedNormal : TEXCOORD9;
    float3 bodyFixedSurfaceDirection : TEXCOORD10;
    float drySurface : TEXCOORD11;
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

struct SurfaceOutputs
{
    float4 previewColor : SV_Target0;
    float4 baseRoughness : SV_Target1;
    float4 normalMetallic : SV_Target2;
    float4 emissionClass : SV_Target3;
};

float EncodeSurfaceMeta(float surfaceClass, float representation)
{
    return surfaceClass + representation / 16.0;
}

SurfaceOutputs main(VSOutput input)
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

    const float drySurface = saturate(input.drySurface);

    const float3 dryOceanColor = float3(0.19, 0.052, 0.012);
    const float3 dryDesertColor = float3(0.43, 0.17, 0.035);
    const float3 dryGrasslandColor = float3(0.35, 0.115, 0.018);
    const float3 dryTemperateColor = float3(0.22, 0.060, 0.012);
    const float3 dryBorealColor = float3(0.17, 0.045, 0.014);
    const float3 dryTundraColor = float3(0.38, 0.20, 0.09);
    const float3 dryAlpineColor = float3(0.47, 0.22, 0.075);
    const float3 dryWetlandColor = float3(0.31, 0.085, 0.014);

    const float3 resolvedOceanColor = lerp(oceanColor, dryOceanColor, drySurface);
    const float3 resolvedDesertColor = lerp(desertColor, dryDesertColor, drySurface);
    const float3 resolvedGrasslandColor = lerp(grasslandColor, dryGrasslandColor, drySurface);
    const float3 resolvedTemperateColor = lerp(temperateForestColor, dryTemperateColor, drySurface);
    const float3 resolvedBorealColor = lerp(borealForestColor, dryBorealColor, drySurface);
    const float3 resolvedTundraColor = lerp(tundraColor, dryTundraColor, drySurface);
    const float3 resolvedAlpineColor = lerp(alpineColor, dryAlpineColor, drySurface);
    const float3 resolvedWetlandColor = lerp(wetlandColor, dryWetlandColor, drySurface);

    float3 color =
        resolvedOceanColor *
            biome0.x +
        resolvedDesertColor *
            biome0.y +
        resolvedGrasslandColor *
            biome0.z +
        resolvedTemperateColor *
            biome0.w +
        resolvedBorealColor *
            biome1.x +
        resolvedTundraColor *
            biome1.y +
        resolvedAlpineColor *
            biome1.z +
        resolvedWetlandColor *
            biome1.w;

    float3 surfaceBaseColor = color;
    float surfaceRoughness = 0.82;
    float surfaceMetallic = 0.0;
    float surfaceClass = 1.0; // SurfaceClass::Terrain

    const float3 terrainNormal =
        ApplyDetailNormal(
            normalize(input.terrainNormal),
            input.worldPosition,
            input.spacingMeters);

    const float3 bodyFixedNormal =
        ApplyDetailNormal(
            normalize(input.bodyFixedNormal),
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

    if (drySurface > 0.5)
    {
        // Direction/body-fixed material frequencies remain stable while the
        // floating origin moves. Broad dust provinces break the uniform red
        // tint; steep faces expose darker basalt beneath the dust mantle.
        const float regional = DetailValueNoise(
            input.worldPosition / 180000.0);
        const float localDust = DetailValueNoise(
            input.worldPosition / 8500.0);
        const float dust = saturate(regional * 0.72 + localDust * 0.28);
        const float exposedRock = saturate((1.0 - slopeCosine) * 4.5);
        const float3 dustyRegolith = lerp(
            float3(0.29, 0.075, 0.012),
            float3(0.49, 0.205, 0.050),
            dust);
        const float3 darkBasalt = float3(0.12, 0.027, 0.010);
        color = lerp(color, dustyRegolith, 0.38);
        color = lerp(color, darkBasalt, exposedRock * 0.58);
        surfaceBaseColor = color;
        surfaceRoughness = lerp(0.92, 0.72, exposedRock);
    }

    // Wet worlds continue to rely on M18/M21 physical exposure. The dry-world
    // branch above is the procedural fallback until crater excavation and
    // dust thickness are carried as explicit GPU material channels.

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
    const float waterCoverage =
        (1.0 - drySurface) * smoothstep(0.0, 0.25, input.waterDepth);
    float3 surfaceNormal = bodyFixedNormal;
    if (waterCoverage > 0.0)
    {
        const float3 viewDirection = normalize(-input.localPosition);
        const float fresnel = pow(1.0 - saturate(dot(surfaceDirection, viewDirection)), 5.0);
        const float shallow = exp2(-max(input.waterDepth, 0.0) / 12.0);
        const float3 waterBaseColor = lerp(
            float3(0.012, 0.075, 0.13),
            float3(0.055, 0.23, 0.25),
            shallow);
        const float3 waterColor = lerp(
            waterBaseColor,
            float3(0.16, 0.34, 0.42),
            fresnel * 0.72);
        color = lerp(color, waterColor, waterCoverage);
        surfaceBaseColor =
            lerp(surfaceBaseColor, waterBaseColor, waterCoverage);
        surfaceRoughness =
            lerp(surfaceRoughness, 0.08, waterCoverage);
        surfaceNormal =
            normalize(
                lerp(
                    bodyFixedNormal,
                    normalize(input.bodyFixedSurfaceDirection),
                    waterCoverage));
        if (waterCoverage >= 0.5)
        {
            surfaceClass = 2.0; // SurfaceClass::Water
        }
    }

    SurfaceOutputs output;
    output.previewColor = float4(color, 1.0);
    output.baseRoughness =
        float4(surfaceBaseColor, surfaceRoughness);
    output.normalMetallic =
        float4(surfaceNormal, surfaceMetallic);
    output.emissionClass =
        float4(
            0.0,
            0.0,
            0.0,
            EncodeSurfaceMeta(surfaceClass, 1.0));
    return output;
}
)";
}
