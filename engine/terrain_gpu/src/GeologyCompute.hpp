#pragma once

namespace orbit::terrain_gpu::detail
{
inline constexpr const char* kGeologyComputeShader = R"(
struct EventPacket
{
    uint4 meta; // kind, age order low, age order high, profile/path offset
    float4 a;   // impact center + radius; flow unused
    float4 b;   // impact east + rim ratio
    float4 c;   // impact north + simple/complex depth ratios
    float4 d;   // ejecta ratio/extent, ray strength/count
    float4 e;   // degradation, age years, prepared elongation, azimuth cosine
    float4 f;   // irregularity, melt, breccia, multiring
    float4 g;   // mass scale, phase, azimuth sine/flow point count, flow width
    float4 h;   // ray extent/flow thickness, ray irregularity/flow age, simple depth, influence cosine
};

struct SegmentPacket
{
    float4 startDirection;
    float4 endDirection;
    float4 midpointDirection;
};

struct GeologyOutput
{
    float impactHeight;
    float iceHeight;
    float excavationDepth;
    float ejectaThickness;
    float debrisField;
    float rayField;
    float meltThickness;
    float brecciaField;
    float resurfacedMaterialFraction;
    float resurfacingThickness;
    float microImpactRoughness;
    float microImpactCoverage;
    float excavationCoverage;
    float formationAgeYears;
    float exposureAgeYears;
    uint formationAgeLow;
    uint formationAgeHigh;
    uint exposureAgeLow;
    uint exposureAgeHigh;
    uint affectingImpacts;
    float iceDamage;
    float fractureCoverage;
    uint nearbySegments;
};

[[vk::binding(0, 0)]] StructuredBuffer<EventPacket> g_events : register(t0);
[[vk::binding(1, 0)]] StructuredBuffer<float4> g_flowPoints : register(t1);
[[vk::binding(2, 0)]] StructuredBuffer<SegmentPacket> g_segments : register(t2);
[[vk::binding(3, 0)]] RWStructuredBuffer<GeologyOutput> g_output : register(u3);

struct PushConstants
{
    uint resolution;
    uint face;
    int tileX;
    int tileY;
    uint tileWidth;
    uint tileHeight;
    uint eventCount;
    uint segmentCount;
    float planetRadius;
    float footprint;
    float surfaceAge;
    float gravity;
    float complexTransitionRadius;
    uint environment;
    uint microImpactCount;
    float microImpactMaximumRadius;
    float microImpactRepresentativeRadius;
    float microImpactPhase;
    float microImpactFrequency;
    uint iceAgeLow;
    uint iceAgeHigh;
    float iceAgeYears;
    float iceWidth;
    float iceGrooveDepth;
    float iceRidgeHeight;
};
[[vk::push_constant]] PushConstants g_pc;

struct ProcessState
{
    float height;
    float excavation;
    float ejecta;
    float debris;
    float rays;
    float melt;
    float breccia;
    float resurfaced;
    float resurfacingThickness;
    float microRoughness;
    float microCoverage;
    float excavationCoverage;
    float formationAgeYears;
    float exposureAgeYears;
    uint formationLow;
    uint formationHigh;
    uint exposureLow;
    uint exposureHigh;
    uint impactCount;
};

float SmoothUnit(float value)
{
    const float x = saturate(value);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

float FeatureWeight(float diameter, float footprint)
{
    const float lower = footprint * 2.0;
    const float upper = footprint * 4.0;
    if (diameter <= lower) return 0.0;
    if (diameter >= upper) return 1.0;
    return SmoothUnit((diameter - lower) / (upper - lower));
}

bool AgeAtLeast(uint low, uint high, uint otherLow, uint otherHigh)
{
    return high > otherHigh || (high == otherHigh && low >= otherLow);
}

float3 CubeDirection(uint face, int x, int y, uint resolution)
{
    const float u = ((float)x + 0.5) / (float)resolution * 2.0 - 1.0;
    const float v = ((float)y + 0.5) / (float)resolution * 2.0 - 1.0;
    float3 cube = float3(0.0, 0.0, 1.0);
    switch (face)
    {
    case 0: cube = float3( 1.0,  v, -u); break;
    case 1: cube = float3(-1.0,  v,  u); break;
    case 2: cube = float3( u,  1.0, -v); break;
    case 3: cube = float3( u, -1.0,  v); break;
    case 4: cube = float3( u,  v,  1.0); break;
    case 5: cube = float3(-u,  v, -1.0); break;
    }
    return normalize(cube);
}

void SurfaceFrame(float3 up, out float3 east, out float3 north)
{
    const float3 reference = abs(up.y) < 0.9999999
        ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    east = normalize(cross(reference, up));
    north = normalize(cross(up, east));
}

float2 SurfaceOffset(float3 center, float3 east, float3 north, float3 sampleDirection, float radius)
{
    const float cosine = clamp(dot(center, sampleDirection), -1.0, 1.0);
    const float angle = acos(cosine);
    if (angle <= 1.0e-7) return 0.0;
    const float3 tangent = sampleDirection - center * cosine;
    const float tangentLength = length(tangent);
    if (tangentLength <= 1.0e-7) return 0.0;
    const float distance = angle * radius;
    const float3 direction = tangent / tangentLength;
    return float2(dot(direction, east), dot(direction, north)) * distance;
}

void ApplyImpact(inout ProcessState state, EventPacket event, float3 direction)
{
    const float radius = event.a.w;
    const float spectral = FeatureWeight(radius * 2.0, g_pc.footprint);
    if (spectral <= 0.0 || event.meta.w == 0xffffffffu) return;
    if (dot(event.a.xyz, direction) < event.h.w) return;

    const float2 offset = SurfaceOffset(event.a.xyz, event.b.xyz, event.c.xyz,
        direction, g_pc.planetRadius);
    const float elongation = event.e.z;
    const float along = offset.x * event.e.w + offset.y * event.g.z;
    const float across = -offset.x * event.g.z + offset.y * event.e.w;
    float x = sqrt((along / (radius * elongation)) * (along / (radius * elongation)) +
        (across / radius) * (across / radius));
    if (x > 1.0e-6 && event.f.x > 0.0)
    {
        const float angle = atan2(across, along);
        const float noise = 0.65 * cos(3.0 * angle + event.g.y) +
            0.35 * cos(5.0 * angle - event.g.y * 0.7);
        x /= max(1.0 + event.f.x * noise, 0.75);
    }
    const float influenceExtent = max(event.d.y,
        event.d.z > 0.0 && event.d.w > 0.0 ? event.h.x : 0.0);
    if (x > influenceExtent) return;

    float degradationRate = 0.0012;
    if (g_pc.environment == 1) degradationRate = 0.012;
    else if (g_pc.environment == 2) degradationRate = 0.004;
    else if (g_pc.environment == 3) degradationRate = 0.05;
    const float elapsed = g_pc.surfaceAge > 0.0
        ? max(g_pc.surfaceAge - event.e.y, 0.0) : event.e.y;
    const float preservation = (1.0 - event.e.x) *
        exp(-degradationRate * (elapsed / 1000000.0)) * spectral;
    if (preservation <= 0.0) return;

    const bool complexProfile = event.meta.w == 2u ||
        (event.meta.w == 0u && radius >= g_pc.complexTransitionRadius *
            sqrt(1.62 / g_pc.gravity));
    const float depthRatio = complexProfile ? event.c.w : event.h.z;
    float excavation = 0.0;
    float craterDelta = 0.0;
    if (x < 1.0)
    {
        const float bowl = max(0.0, 1.0 - x * x);
        excavation = radius * depthRatio * bowl * bowl;
        craterDelta = -excavation;
        if (complexProfile)
        {
            const float transition = g_pc.complexTransitionRadius *
                sqrt(1.62 / g_pc.gravity);
            const float peakRatio = clamp(0.025 + 0.02 * radius / max(transition, 1.0),
                0.025, 0.085);
            if (x < 0.28)
            {
                const float peakX = 1.0 - x / 0.28;
                craterDelta += radius * min(peakRatio, depthRatio * 0.85) * peakX * peakX;
            }
            if (x > 0.62)
            {
                const float phase = (x - 0.62) / 0.38;
                craterDelta += radius * 0.012 * clamp(sqrt(1.62 / g_pc.gravity), 0.65, 1.4) *
                    sin(phase * 3.0 * 3.14159265358979323846) * (1.0 - phase);
            }
        }
    }

    const float rim = x <= event.d.y
        ? radius * event.b.w * exp(-0.5 * ((x - 1.0) / 0.10) * ((x - 1.0) / 0.10)) : 0.0;
    if (event.f.w > 0.0 && radius >= 5.0 * g_pc.complexTransitionRadius &&
        x >= 1.0 && x <= event.d.y)
    {
        const float phase = (x - 1.0) * (2.0 * 3.14159265358979323846 / 1.35);
        craterDelta += radius * event.f.w * 0.012 * cos(phase) * exp(-(x - 1.0) * 0.45);
    }

    float ejecta = 0.0;
    float ray = 0.0;
    float angularRay = 0.0;
    if (x >= 1.0 && event.d.z > 0.0 && event.d.w > 0.0)
    {
        const float azimuth = atan2(offset.y, offset.x);
        const float distortion = event.h.y * (
            0.65 * sin(3.0 * azimuth + x * 0.45 + event.g.y) +
            0.35 * sin(5.0 * azimuth - x * 0.23 - event.g.y));
        angularRay = event.d.z * pow(max(0.0,
            cos((azimuth + distortion) * event.d.w + event.g.y)), 8.0);
        if (event.h.x > 0.0 && x <= event.h.x)
            ray = angularRay * (1.0 - SmoothUnit((x - 1.0) / max(event.h.x - 1.0, 1.0e-9)));
    }
    if (x >= 1.0 && x <= event.d.y)
    {
        const float extent = saturate((x - 1.0) / max(event.d.y - 1.0, 1.0e-9));
        const float outerFade = 1.0 - SmoothUnit(extent);
        ejecta = radius * event.d.x * pow(max(x, 1.0), -3.0) * outerFade * event.g.x;
        if (event.h.x == 0.0) ray = angularRay;
        ejecta *= 1.0 + angularRay;
    }
    const float impactHeight = (craterDelta + rim + ejecta) * preservation;
    const float coverage = x < 1.0
        ? saturate((1.0 - x * x) * (1.0 - x * x) * preservation) : 0.0;
    const float retained = 1.0 - coverage;
    state.height = state.height * retained + impactHeight;
    state.excavation = state.excavation * retained + excavation * preservation;
    state.ejecta = state.ejecta * retained + ejecta * preservation;
    state.melt = state.melt * retained + excavation * event.f.y * preservation;
    state.breccia = saturate(state.breccia * retained + saturate(
        ((ejecta * preservation + rim * preservation * 0.35) /
            max(radius * max(event.d.x, 0.001), 1.0)) * event.f.z));
    state.debris = saturate(state.debris * retained + saturate(
        (ejecta * preservation + rim * preservation * 0.35) /
            max(radius * max(event.d.x, 0.001), 1.0)));
    state.rays = saturate(state.rays * retained + ray * preservation);
    state.resurfacingThickness *= retained;
    state.resurfaced *= retained;
    state.excavationCoverage = 1.0 - (1.0 - state.excavationCoverage) * retained;
    if (state.impactCount == 0u || coverage >= 0.95)
    {
        state.formationLow = event.meta.y;
        state.formationHigh = event.meta.z;
        state.formationAgeYears = event.e.y;
    }
    if (coverage > 0.01 || ejecta > 0.0 || ray > 0.0)
    {
        state.exposureLow = event.meta.y;
        state.exposureHigh = event.meta.z;
        state.exposureAgeYears = elapsed;
    }
    state.impactCount += 1u;
}
)" R"(

void ApplyResurfacing(inout ProcessState state, EventPacket event, float3 direction)
{
    if (event.meta.x == 0xffffffffu) return;
    const uint pointCount = (uint)event.g.z;
    if (pointCount < 2u) return;
    float minimumDistance = 3.402823466e+38;
    [loop] for (uint pointIndex = 1u; pointIndex < pointCount; ++pointIndex)
    {
        const float3 aDirection = normalize(g_flowPoints[event.meta.w + pointIndex - 1u].xyz);
        const float3 bDirection = normalize(g_flowPoints[event.meta.w + pointIndex].xyz);
        const float3 midpoint = normalize(aDirection + bDirection);
        float3 east, north;
        SurfaceFrame(midpoint, east, north);
        const float2 a = SurfaceOffset(midpoint, east, north, aDirection, g_pc.planetRadius);
        const float2 b = SurfaceOffset(midpoint, east, north, bDirection, g_pc.planetRadius);
        const float2 p = SurfaceOffset(midpoint, east, north, direction, g_pc.planetRadius);
        const float2 edge = b - a;
        const float lengthSquared = dot(edge, edge);
        const float t = lengthSquared > 1.0e-9
            ? saturate(dot(p - a, edge) / lengthSquared) : 0.5;
        minimumDistance = min(minimumDistance, length(p - (a + edge * t)));
    }
    const float halfWidth = event.g.w * 0.5;
    const float coverage = 1.0 - SmoothUnit((minimumDistance - halfWidth) / max(halfWidth, 1.0));
    if (coverage <= 0.0) return;
    const float thickness = event.h.x;
    if (event.meta.x == 3u)
    {
        const float coreWidth = max(halfWidth * 0.22, 1.0);
        const float shoulder = (minimumDistance - halfWidth * 0.62) / max(halfWidth * 0.12, 1.0);
        const float ridge = exp(-0.5 * (minimumDistance / coreWidth) * (minimumDistance / coreWidth));
        const float trough = exp(-0.5 * shoulder * shoulder);
        const float retained = 1.0 - 0.22 * coverage;
        state.height = state.height * retained + thickness * coverage * (ridge - 0.28 * trough);
        state.excavation *= retained;
        state.ejecta *= retained;
        state.melt *= retained;
        state.resurfacingThickness *= retained;
        state.rays *= retained;
        state.breccia = saturate(state.breccia * retained + 0.3 * coverage);
        state.debris = saturate(state.debris * retained + 0.16 * coverage);
        state.ejecta += 0.04 * thickness * coverage;
        state.excavationCoverage = max(state.excavationCoverage, 0.22 * coverage);
        if (AgeAtLeast(event.meta.y, event.meta.z, state.exposureLow, state.exposureHigh))
        {
            state.exposureLow = event.meta.y;
            state.exposureHigh = event.meta.z;
            state.exposureAgeYears = max(0.0, g_pc.surfaceAge - event.h.y);
        }
        return;
    }
    const float retained = 1.0 - coverage;
    state.height = state.height * retained + thickness * coverage;
    state.excavation *= retained;
    state.ejecta *= retained;
    state.melt *= retained;
    state.breccia *= retained;
    state.debris *= retained;
    state.rays *= retained;
    state.resurfacingThickness = state.resurfacingThickness * retained + thickness * coverage;
    state.resurfaced = saturate(state.resurfaced * retained + coverage);
    if (AgeAtLeast(event.meta.y, event.meta.z, state.exposureLow, state.exposureHigh))
    {
        state.exposureLow = event.meta.y;
        state.exposureHigh = event.meta.z;
        state.exposureAgeYears = max(0.0, g_pc.surfaceAge - event.h.y);
        if (coverage >= 0.95)
        {
            state.formationLow = event.meta.y;
            state.formationHigh = event.meta.z;
            state.formationAgeYears = event.h.y;
        }
    }
}

void MainSample(uint3 dispatchId)
{
    if (dispatchId.x >= g_pc.tileWidth || dispatchId.y >= g_pc.tileHeight) return;
    const uint outputIndex = dispatchId.y * g_pc.tileWidth + dispatchId.x;
    const int x = g_pc.tileX + (int)dispatchId.x;
    const int y = g_pc.tileY + (int)dispatchId.y;
    const float3 direction = CubeDirection(g_pc.face, x, y, g_pc.resolution);
    ProcessState state;
    state.height = 0.0;
    state.excavation = 0.0;
    state.ejecta = 0.0;
    state.debris = 0.0;
    state.rays = 0.0;
    state.melt = 0.0;
    state.breccia = 0.0;
    state.resurfaced = 0.0;
    state.resurfacingThickness = 0.0;
    state.microRoughness = 0.0;
    state.microCoverage = 0.0;
    state.excavationCoverage = 0.0;
    state.formationAgeYears = 0.0;
    state.exposureAgeYears = 0.0;
    state.formationLow = 0u;
    state.formationHigh = 0u;
    state.exposureLow = 0u;
    state.exposureHigh = 0u;
    state.impactCount = 0u;
    [loop] for (uint eventIndex = 0u; eventIndex < g_pc.eventCount; ++eventIndex)
    {
        const EventPacket event = g_events[eventIndex];
        if (event.meta.x == 0u) ApplyImpact(state, event, direction);
        else ApplyResurfacing(state, event, direction);
    }

    if (g_pc.microImpactCount > 0u && g_pc.microImpactMaximumRadius > 0.0)
    {
        const float surfaceArea = 4.0 * 3.14159265358979323846 *
            g_pc.planetRadius * g_pc.planetRadius;
        const float expected = (float)g_pc.microImpactCount *
            g_pc.footprint * g_pc.footprint / max(surfaceArea, 1.0);
        const bool hasExposure = state.exposureLow != 0u || state.exposureHigh != 0u ||
            state.excavationCoverage > 0.01 || state.ejecta > 0.0 ||
            state.resurfaced > 0.0 || state.rays > 0.0;
        const float exposure = hasExposure
            ? state.exposureAgeYears : g_pc.surfaceAge;
        const float ageFraction = g_pc.surfaceAge > 0.0
            ? saturate(exposure / g_pc.surfaceAge) : 1.0;
        state.microCoverage = 1.0 - exp(-expected * ageFraction);
        const float microRadius = g_pc.microImpactRepresentativeRadius;
        state.microRoughness = min(microRadius * 0.22 * sqrt(state.microCoverage),
            g_pc.footprint * 0.12);
        const float residualWeight = FeatureWeight(microRadius * 2.0, g_pc.footprint);
        if (residualWeight > 0.0)
        {
            const float noise = 0.25 * (
                sin((direction.x + direction.y * 0.37) * g_pc.microImpactFrequency + g_pc.microImpactPhase) +
                sin((direction.y + direction.z * 0.41) * g_pc.microImpactFrequency - g_pc.microImpactPhase * 0.7) +
                cos((direction.z + direction.x * 0.29) * g_pc.microImpactFrequency + g_pc.microImpactPhase * 1.3) +
                cos((direction.x - direction.y + direction.z) * g_pc.microImpactFrequency * 0.73));
            state.height += state.microRoughness * noise * residualWeight;
        }
    }

    float iceHeight = 0.0;
    float iceDamage = 0.0;
    uint nearbySegments = 0u;
    if (g_pc.iceWidth > 0.0 && g_pc.segmentCount > 0u &&
        FeatureWeight(g_pc.iceWidth * 4.0, g_pc.footprint) > 0.0)
    {
        const float featureWeight = FeatureWeight(g_pc.iceWidth * 4.0, g_pc.footprint);
        [loop] for (uint segmentIndex = 0u; segmentIndex < g_pc.segmentCount; ++segmentIndex)
        {
            const SegmentPacket segment = g_segments[segmentIndex];
            const float3 midpoint = normalize(segment.midpointDirection.xyz);
            float3 east, north;
            SurfaceFrame(midpoint, east, north);
            const float2 a = SurfaceOffset(midpoint, east, north,
                normalize(segment.startDirection.xyz), g_pc.planetRadius);
            const float2 b = SurfaceOffset(midpoint, east, north,
                normalize(segment.endDirection.xyz), g_pc.planetRadius);
            const float2 p = SurfaceOffset(midpoint, east, north, direction, g_pc.planetRadius);
            const float2 edge = b - a;
            const float edgeLengthSquared = dot(edge, edge);
            const float t = edgeLengthSquared > 1.0e-9
                ? saturate(dot(p - a, edge) / edgeLengthSquared) : 0.5;
            const float distance = length(p - (a + edge * t));
            if (distance > 4.0 * g_pc.iceWidth) continue;
            const float normalized = distance / g_pc.iceWidth;
            const float groove = exp(-0.5 * normalized * normalized);
            const float ridgeOffset = normalized - 2.2;
            const float ridge = exp(-0.5 * ridgeOffset * ridgeOffset);
            iceHeight += featureWeight * (-g_pc.iceGrooveDepth * groove +
                g_pc.iceRidgeHeight * ridge);
            iceDamage = max(iceDamage, saturate(groove * featureWeight));
            ++nearbySegments;
        }
    }
    const bool fracturesAfterSurface = iceDamage > 0.0 &&
        AgeAtLeast(g_pc.iceAgeLow, g_pc.iceAgeHigh, state.exposureLow, state.exposureHigh);
    const float fractureRetention = fracturesAfterSurface || iceDamage <= 0.0
        ? 1.0 : (1.0 - state.resurfaced) * (1.0 - state.excavationCoverage);
    if (fracturesAfterSurface)
    {
        state.exposureLow = g_pc.iceAgeLow;
        state.exposureHigh = g_pc.iceAgeHigh;
        state.exposureAgeYears = max(0.0, g_pc.surfaceAge - g_pc.iceAgeYears);
    }

    GeologyOutput output;
    output.impactHeight = state.height;
    output.iceHeight = iceHeight * fractureRetention;
    output.excavationDepth = state.excavation;
    output.ejectaThickness = state.ejecta;
    output.debrisField = state.debris;
    output.rayField = state.rays;
    output.meltThickness = state.melt;
    output.brecciaField = state.breccia;
    output.resurfacedMaterialFraction = state.resurfaced;
    output.resurfacingThickness = state.resurfacingThickness;
    output.microImpactRoughness = state.microRoughness;
    output.microImpactCoverage = state.microCoverage;
    output.excavationCoverage = state.excavationCoverage;
    output.formationAgeYears = state.formationAgeYears;
    output.exposureAgeYears = state.exposureAgeYears;
    output.formationAgeLow = state.formationLow;
    output.formationAgeHigh = state.formationHigh;
    output.exposureAgeLow = state.exposureLow;
    output.exposureAgeHigh = state.exposureHigh;
    output.affectingImpacts = state.impactCount;
    output.iceDamage = iceDamage * fractureRetention;
    output.fractureCoverage = iceDamage * fractureRetention;
    output.nearbySegments = nearbySegments;
    g_output[outputIndex] = output;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    MainSample(dispatchId);
}
)";
} // namespace orbit::terrain_gpu::detail
