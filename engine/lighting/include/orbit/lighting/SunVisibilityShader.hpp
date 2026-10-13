#pragma once

namespace orbit::lighting
{
// Shared body-fixed terrain sun traversal for surfaces, air and clouds.
// No camera, screen depth, viewport bounds or render LOD enters visibility.
inline constexpr const char* kSunVisibilityShader = R"(
float StellarDiscFraction(float clearance, float angularRadius)
{
    if (angularRadius <= 1.0e-7) return clearance >= 0.0 ? 1.0 : 0.0;
    const float x = clamp(clearance / angularRadius, -1.0, 1.0);
    return saturate(0.5 + (asin(x) + x * sqrt(max(1.0 - x * x, 0.0))) / 3.14159265359);
}

float SphereStellarVisibility(float3 position, float3 sun, float radius, float angularRadius)
{
    const float distance = length(position);
    if (radius <= 0.0) return 1.0;
    if (distance <= radius) return 0.0;
    const float planetAngle = asin(clamp(radius / distance, 0.0, 1.0));
    const float separation = acos(clamp(dot(-position / distance, sun), -1.0, 1.0));
    if (angularRadius <= 1.0e-7) return separation >= planetAngle ? 1.0 : 0.0;
    // The straight-edge limit avoids catastrophic cancellation when the
    // planet's disc is hundreds of times larger than the stellar disc.
    if (planetAngle > angularRadius * 10.0)
        return StellarDiscFraction(separation - planetAngle, angularRadius);
    if (separation >= planetAngle + angularRadius) return 1.0;
    if (planetAngle >= separation + angularRadius) return 0.0;
    if (angularRadius >= separation + planetAngle)
        return saturate(1.0 - planetAngle * planetAngle / (angularRadius * angularRadius));
    const float a2 = angularRadius * angularRadius, b2 = planetAngle * planetAngle;
    const float d2 = separation * separation;
    const float a = acos(clamp((d2 + a2 - b2) / (2.0 * separation * angularRadius), -1.0, 1.0));
    const float b = acos(clamp((d2 + b2 - a2) / (2.0 * separation * planetAngle), -1.0, 1.0));
    const float root = sqrt(max((-separation + angularRadius + planetAngle) *
        (separation + angularRadius - planetAngle) * (separation - angularRadius + planetAngle) *
        (separation + angularRadius + planetAngle), 0.0));
    return saturate(1.0 - (a2 * a + b2 * b - 0.5 * root) / (3.14159265359 * a2));
}

float3 AtmosphereStellarTransmittance(Texture2D table, SamplerState tableSampler,
    float radius, float bottom, float top, float sunMu, float angularRadius)
{
    if (top <= bottom || bottom <= 0.0) return 1.0;
    const float radial = max(radius, bottom);
    const float horizon = -sqrt(max(1.0 - (bottom / radial) * (bottom / radial), 0.0));
    const float spread = angularRadius * sqrt(max(1.0 - sunMu * sunMu, 0.0));
    float effectiveMu = sunMu;
    if (spread > 1.0e-7)
    {
        const float lower = clamp((horizon - sunMu) / spread, -1.0, 1.0);
        const float visible = StellarDiscFraction(sunMu - horizon, spread);
        // Conditional discCentre of the unblocked part of a uniform stellar disc.
        // Body/terrain visibility supplies its area; this lookup supplies only
        // atmospheric extinction, so the horizon is not multiplied twice.
        const float discCentre = 2.0 * pow(max(1.0 - lower * lower, 0.0), 1.5) /
            max(3.0 * 3.14159265359 * visible, 1.0e-6);
        effectiveMu += spread * discCentre;
    }
    effectiveMu = max(effectiveMu, horizon + 1.0e-5);
    const float unit = sqrt(saturate((radial - bottom) / max(top - bottom, 1.0)));
    return table.SampleLevel(tableSampler, float2(saturate(effectiveMu * 0.5 + 0.5), unit), 0).rgb;
}

float PlanetSunVisibility(float3 position, float3 sun, float radius)
{
    if (radius <= 0.0) return 1.0;
    const float along = dot(position, sun);
    if (along >= 0.0) return 1.0;
    const float3 perpendicular = position - along * sun;
    const float biasedRadius = max(radius - max(0.05, radius * 2.0e-7), 0.0);
    return dot(perpendicular, perpendicular) < biasedRadius * biasedRadius ? 0.0 : 1.0;
}

float TerrainShadowElevation(ByteAddressBuffer field, float3 position, uint width, uint height)
{
    const float3 d = normalize(position);
    const float x = (atan2(d.z, d.x) / 6.28318530718 + 0.5) * float(width) - 0.5;
    const float y = clamp((asin(clamp(d.y, -1.0, 1.0)) / 3.14159265359 + 0.5) * float(height) - 0.5,
                          0.0, float(height - 1u));
    const int ix = int(floor(x));
    const uint x0 = uint((ix % int(width) + int(width)) % int(width));
    const uint x1 = (x0 + 1u) % width;
    const uint y0 = uint(floor(y)), y1 = min(y0 + 1u, height - 1u);
    const float a = asfloat(field.Load(32u + (y0 * width + x0) * 4u));
    const float b = asfloat(field.Load(32u + (y0 * width + x1) * 4u));
    const float c = asfloat(field.Load(32u + (y1 * width + x0) * 4u));
    const float e = asfloat(field.Load(32u + (y1 * width + x1) * 4u));
    return lerp(lerp(a, b, frac(x)), lerp(c, e, frac(x)), frac(y));
}

float TerrainSunVisibility(ByteAddressBuffer field, float3 position, float3 sun, float fallbackRadius, float angularRadius)
{
    const uint width = field.Load(12u), height = field.Load(16u);
    if (width == 0u || height == 0u)
        return angularRadius > 0.0 ? SphereStellarVisibility(position, sun, fallbackRadius, angularRadius) :
            PlanetSunVisibility(position, sun, fallbackRadius);
    const float radius = asfloat(field.Load(0u));
    const float minimumElevation = asfloat(field.Load(4u));
    const float maximumElevation = asfloat(field.Load(8u));
    const float bias = max(0.5, radius * 2.0e-7);
    const float along = dot(position, sun);
    const float3 perpendicular = position - along * sun;
    const float perpendicular2 = dot(perpendicular, perpendicular);
    const float inner = radius + minimumElevation - bias;
    float visibility = angularRadius > 0.0 ? SphereStellarVisibility(position, sun, inner, angularRadius) : 1.0;
    if (visibility <= 0.0) return 0.0;
    if (angularRadius <= 0.0 && along < 0.0 && perpendicular2 < inner * inner) return 0.0;
    const float outer = radius + maximumElevation + bias + length(position) * sin(angularRadius);
    const float discriminant = outer * outer - perpendicular2;
    if (discriminant <= 0.0) return visibility;
    const float root = sqrt(discriminant);
    const float start = max(-along - root, 0.0), end = -along + root;
    if (end <= start) return visibility;
    const float stepLength = max(asfloat(field.Load(20u)) * 0.5, (end - start) / 30.0);
    // Samples lie on planes fixed to the body origin along the sun axis.
    // Camera movement or a caster leaving the framebuffer cannot move them.
    float t = max(start, ceil((along + start) / stepLength) * stepLength - along);
    [loop]
    for (uint i = 0u; i < 32u && t <= end; ++i, t += stepLength)
    {
        const float3 probe = position + sun * t;
        const float surface = radius + TerrainShadowElevation(field, probe, width, height);
        const float clearance = length(probe) - surface + bias;
        if (angularRadius <= 0.0)
        {
            if (clearance < 0.0) return 0.0;
        }
        else if (t > bias * 4.0)
        {
            visibility = min(visibility, StellarDiscFraction(atan2(clearance, t), angularRadius));
            if (visibility <= 0.0) return 0.0;
        }
    }
    return visibility;
}
)";
} // namespace orbit::lighting
