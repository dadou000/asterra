#pragma once

namespace orbit::terrain_gpu::detail
{
inline constexpr const char* kM17InitializeShader = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_waterOut : register(u0);
[[vk::binding(1, 0)]]
RWByteAddressBuffer g_momentumOut : register(u1);

[[vk::binding(2, 0)]]
RWTexture2D<float> g_bedrock : register(u2);
[[vk::binding(3, 0)]]
RWTexture2D<float4> g_loose : register(u3);

struct PushConstants
{
    uint resolution;
    float seaLevelMeters;
};

[[vk::push_constant]] PushConstants g_pc;

float Surface(uint2 p)
{
    const float4 loose = g_loose[p];
    return g_bedrock[p] + loose.x + loose.y + loose.z + loose.w;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = id.y * g_pc.resolution + id.x;
    const float depth = max(g_pc.seaLevelMeters - Surface(id.xy), 0.0);

    g_waterOut.Store(index * 4u, asuint(depth));

    const uint mo = index * 8u;
    g_momentumOut.Store(mo + 0u, asuint(0.0));
    g_momentumOut.Store(mo + 4u, asuint(0.0));
}
)";

inline constexpr const char* kM17AdvanceShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_waterIn : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_momentumIn : register(t1);
[[vk::binding(2, 0)]]
ByteAddressBuffer g_boundary : register(t2);
[[vk::binding(3, 0)]]
RWByteAddressBuffer g_waterOut : register(u3);
[[vk::binding(4, 0)]]
RWByteAddressBuffer g_momentumOut : register(u4);

[[vk::binding(5, 0)]]
RWTexture2D<float> g_bedrock : register(u5);
[[vk::binding(6, 0)]]
RWTexture2D<float4> g_loose : register(u6);

struct PushConstants
{
    uint resolution;
    float spacingMeters;
    float timeStepSeconds;
    float gravity;
    float dryThresholdMeters;
    float wetThresholdMeters;
    float manningRoughness;
    float maximumVelocityMetersPerSecond;
    float elapsedSeconds;
    float waveAmplitudeMeters;
    float wavePeriodSeconds;
    float wavePhaseRadians;
    float waveDirectionX;
    float waveDirectionY;
};

[[vk::push_constant]] PushConstants g_pc;

struct State
{
    float h;
    float qx;
    float qy;
};

struct FluxPair
{
    State left;
    State right;
};

float Surface(uint2 p)
{
    const float4 loose = g_loose[p];
    return g_bedrock[p] + loose.x + loose.y + loose.z + loose.w;
}

State LoadState(uint2 p)
{
    const uint index = p.y * g_pc.resolution + p.x;
    const uint mo = index * 8u;

    State s;
    s.h = max(asfloat(g_waterIn.Load(index * 4u)), 0.0);
    s.qx = asfloat(g_momentumIn.Load(mo + 0u));
    s.qy = asfloat(g_momentumIn.Load(mo + 4u));
    return s;
}

State ScaleDepth(State source, float depth)
{
    State result;
    result.h = max(depth, 0.0);

    if (source.h <= g_pc.dryThresholdMeters ||
        result.h <= g_pc.dryThresholdMeters)
    {
        result.qx = 0.0;
        result.qy = 0.0;
        return result;
    }

    const float ratio = result.h / source.h;
    result.qx = source.qx * ratio;
    result.qy = source.qy * ratio;
    return result;
}

State FluxX(State state)
{
    State result = (State)0;

    if (state.h <= g_pc.dryThresholdMeters)
    {
        return result;
    }

    const float u = state.qx / state.h;
    const float v = state.qy / state.h;

    result.h = state.qx;
    result.qx = state.qx * u + 0.5 * g_pc.gravity * state.h * state.h;
    result.qy = state.qx * v;
    return result;
}

State FluxY(State state)
{
    State result = (State)0;

    if (state.h <= g_pc.dryThresholdMeters)
    {
        return result;
    }

    const float u = state.qx / state.h;
    const float v = state.qy / state.h;

    result.h = state.qy;
    result.qx = state.qy * u;
    result.qy = state.qy * v + 0.5 * g_pc.gravity * state.h * state.h;
    return result;
}

FluxPair InterfaceX(State left, float leftBed, State right, float rightBed)
{
    const float leftSurface = leftBed + left.h;
    const float rightSurface = rightBed + right.h;
    const float z = max(leftBed, rightBed);

    const State ls = ScaleDepth(left, max(leftSurface - z, 0.0));
    const State rs = ScaleDepth(right, max(rightSurface - z, 0.0));

    const State fl = FluxX(ls);
    const State fr = FluxX(rs);

    const float ul = ls.h > g_pc.dryThresholdMeters ? ls.qx / ls.h : 0.0;
    const float ur = rs.h > g_pc.dryThresholdMeters ? rs.qx / rs.h : 0.0;

    const float signal = max(
        abs(ul) + sqrt(g_pc.gravity * max(ls.h, 0.0)),
        abs(ur) + sqrt(g_pc.gravity * max(rs.h, 0.0)));

    State f;
    f.h = 0.5 * (fl.h + fr.h) - 0.5 * signal * (rs.h - ls.h);
    f.qx = 0.5 * (fl.qx + fr.qx) - 0.5 * signal * (rs.qx - ls.qx);
    f.qy = 0.5 * (fl.qy + fr.qy) - 0.5 * signal * (rs.qy - ls.qy);

    const float lc = 0.5 * g_pc.gravity *
        (left.h * left.h - ls.h * ls.h);
    const float rc = 0.5 * g_pc.gravity *
        (right.h * right.h - rs.h * rs.h);

    FluxPair result;
    result.left = f;
    result.right = f;
    result.left.qx += lc;
    result.right.qx += rc;
    return result;
}

FluxPair InterfaceY(State north, float northBed, State south, float southBed)
{
    const float northSurface = northBed + north.h;
    const float southSurface = southBed + south.h;
    const float z = max(northBed, southBed);

    const State ns = ScaleDepth(north, max(northSurface - z, 0.0));
    const State ss = ScaleDepth(south, max(southSurface - z, 0.0));

    const State fn = FluxY(ns);
    const State fs = FluxY(ss);

    const float vn = ns.h > g_pc.dryThresholdMeters ? ns.qy / ns.h : 0.0;
    const float vs = ss.h > g_pc.dryThresholdMeters ? ss.qy / ss.h : 0.0;

    const float signal = max(
        abs(vn) + sqrt(g_pc.gravity * max(ns.h, 0.0)),
        abs(vs) + sqrt(g_pc.gravity * max(ss.h, 0.0)));

    State f;
    f.h = 0.5 * (fn.h + fs.h) - 0.5 * signal * (ss.h - ns.h);
    f.qx = 0.5 * (fn.qx + fs.qx) - 0.5 * signal * (ss.qx - ns.qx);
    f.qy = 0.5 * (fn.qy + fs.qy) - 0.5 * signal * (ss.qy - ns.qy);

    const float nc = 0.5 * g_pc.gravity *
        (north.h * north.h - ns.h * ns.h);
    const float sc = 0.5 * g_pc.gravity *
        (south.h * south.h - ss.h * ss.h);

    FluxPair result;
    result.left = f;
    result.right = f;
    result.left.qy += nc;
    result.right.qy += sc;
    return result;
}

// Boundary layout: north, east, south, west. Each record is 20 bytes:
// bed, still surface, vx, vy, mode.
uint BoundaryOffset(uint side, uint edge)
{
    return (side * g_pc.resolution + edge) * 20u;
}

uint BoundaryMode(uint side, uint edge)
{
    return g_boundary.Load(BoundaryOffset(side, edge) + 16u);
}

float BoundaryBed(uint side, uint edge, float sourceBed)
{
    if (BoundaryMode(side, edge) == 0u)
    {
        return sourceBed;
    }

    return asfloat(g_boundary.Load(BoundaryOffset(side, edge) + 0u));
}

float2 BoundaryPosition(uint side, uint edge)
{
    const float halfCells = 0.5 * float(g_pc.resolution - 1u);
    float2 p = float2(
        (float(edge) - halfCells) * g_pc.spacingMeters,
        (float(edge) - halfCells) * g_pc.spacingMeters);

    if (side == 0u)
    {
        p.y = -halfCells * g_pc.spacingMeters;
    }
    else if (side == 1u)
    {
        p.x = halfCells * g_pc.spacingMeters;
    }
    else if (side == 2u)
    {
        p.y = halfCells * g_pc.spacingMeters;
    }
    else
    {
        p.x = -halfCells * g_pc.spacingMeters;
    }

    return p;
}

State Ghost(
    uint side,
    uint edge,
    State source,
    float sourceBed)
{
    const uint mode = BoundaryMode(side, edge);

    if (mode == 0u)
    {
        State result = source;

        if (side == 1u || side == 3u)
        {
            result.qx = -result.qx;
        }
        else
        {
            result.qy = -result.qy;
        }

        return result;
    }

    const uint o = BoundaryOffset(side, edge);
    const float bed = asfloat(g_boundary.Load(o + 0u));
    float surface = asfloat(g_boundary.Load(o + 4u));
    float2 velocity = float2(
        asfloat(g_boundary.Load(o + 8u)),
        asfloat(g_boundary.Load(o + 12u)));

    if (mode == 1u && g_pc.waveAmplitudeMeters > 0.0)
    {
        const float2 direction = normalize(float2(
            g_pc.waveDirectionX,
            g_pc.waveDirectionY));

        const float stillDepth = max(
            surface - bed,
            g_pc.wetThresholdMeters);

        const float c = sqrt(g_pc.gravity * stillDepth);
        const float omega = 6.283185307179586 / max(g_pc.wavePeriodSeconds, 1.0e-6);
        const float k = omega / max(c, 1.0e-6);
        const float2 p = BoundaryPosition(side, edge);

        const float phase =
            omega * g_pc.elapsedSeconds -
            k * dot(direction, p) +
            g_pc.wavePhaseRadians;

        const float eta =
            g_pc.waveAmplitudeMeters * sin(phase);

        surface += eta;

        const float linearVelocity =
            eta * sqrt(g_pc.gravity / stillDepth);

        velocity += direction * linearVelocity;
    }

    State result;
    result.h = max(surface - bed, 0.0);
    result.qx = result.h * velocity.x;
    result.qy = result.h * velocity.y;

    if (result.h <= g_pc.dryThresholdMeters)
    {
        result = (State)0;
    }

    return result;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint2 p = id.xy;
    const uint index = id.y * g_pc.resolution + id.x;
    const State source = LoadState(p);
    const float bed = Surface(p);

    State west;
    float westBed;

    if (id.x > 0u)
    {
        west = LoadState(uint2(id.x - 1u, id.y));
        westBed = Surface(uint2(id.x - 1u, id.y));
    }
    else
    {
        west = Ghost(3u, id.y, source, bed);
        westBed = BoundaryBed(3u, id.y, bed);
    }

    State east;
    float eastBed;

    if (id.x + 1u < g_pc.resolution)
    {
        east = LoadState(uint2(id.x + 1u, id.y));
        eastBed = Surface(uint2(id.x + 1u, id.y));
    }
    else
    {
        east = Ghost(1u, id.y, source, bed);
        eastBed = BoundaryBed(1u, id.y, bed);
    }

    State north;
    float northBed;

    if (id.y > 0u)
    {
        north = LoadState(uint2(id.x, id.y - 1u));
        northBed = Surface(uint2(id.x, id.y - 1u));
    }
    else
    {
        north = Ghost(0u, id.x, source, bed);
        northBed = BoundaryBed(0u, id.x, bed);
    }

    State south;
    float southBed;

    if (id.y + 1u < g_pc.resolution)
    {
        south = LoadState(uint2(id.x, id.y + 1u));
        southBed = Surface(uint2(id.x, id.y + 1u));
    }
    else
    {
        south = Ghost(2u, id.x, source, bed);
        southBed = BoundaryBed(2u, id.x, bed);
    }

    const FluxPair fw = InterfaceX(west, westBed, source, bed);
    const FluxPair fe = InterfaceX(source, bed, east, eastBed);
    const FluxPair fn = InterfaceY(north, northBed, source, bed);
    const FluxPair fs = InterfaceY(source, bed, south, southBed);

    const float scale = g_pc.timeStepSeconds / max(g_pc.spacingMeters, 1.0e-6);

    State updated;
    updated.h = source.h - scale *
        ((fe.left.h - fw.right.h) +
         (fs.left.h - fn.right.h));

    updated.qx = source.qx - scale *
        ((fe.left.qx - fw.right.qx) +
         (fs.left.qx - fn.right.qx));

    updated.qy = source.qy - scale *
        ((fe.left.qy - fw.right.qy) +
         (fs.left.qy - fn.right.qy));

    if (updated.h <= g_pc.dryThresholdMeters)
    {
        updated = (State)0;
    }
    else
    {
        updated.h = max(updated.h, 0.0);

        float2 velocity = float2(updated.qx, updated.qy) / updated.h;
        const float speed = length(velocity);

        if (speed > g_pc.maximumVelocityMetersPerSecond)
        {
            velocity *= g_pc.maximumVelocityMetersPerSecond / max(speed, 1.0e-6);
            updated.qx = velocity.x * updated.h;
            updated.qy = velocity.y * updated.h;
        }

        const float dampSpeed = length(velocity);
        const float depthTerm = pow(
            max(updated.h, g_pc.wetThresholdMeters),
            4.0 / 3.0);

        const float damping = 1.0 / (
            1.0 +
            g_pc.timeStepSeconds *
            g_pc.gravity *
            g_pc.manningRoughness *
            g_pc.manningRoughness *
            dampSpeed /
            max(depthTerm, 1.0e-6));

        updated.qx *= damping;
        updated.qy *= damping;
    }

    g_waterOut.Store(index * 4u, asuint(updated.h));

    const uint mo = index * 8u;
    g_momentumOut.Store(mo + 0u, asuint(updated.qx));
    g_momentumOut.Store(mo + 4u, asuint(updated.qy));
}
)";

inline constexpr const char* kM17ShorelineShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_water : register(t0);
[[vk::binding(1, 0)]]
RWByteAddressBuffer g_wet : register(u1);
[[vk::binding(2, 0)]]
RWByteAddressBuffer g_shoreline : register(u2);

struct PushConstants
{
    uint resolution;
    float wetThresholdMeters;
};

[[vk::push_constant]] PushConstants g_pc;

bool Wet(int2 p)
{
    if (p.x < 0 || p.y < 0 ||
        p.x >= int(g_pc.resolution) ||
        p.y >= int(g_pc.resolution))
    {
        return false;
    }

    const uint index = uint(p.y) * g_pc.resolution + uint(p.x);
    return asfloat(g_water.Load(index * 4u)) >= g_pc.wetThresholdMeters;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = id.y * g_pc.resolution + id.x;
    const bool wet = Wet(int2(id.xy));

    bool shoreline = false;

    const int2 offsets[4] = {
        int2(-1, 0),
        int2(1, 0),
        int2(0, -1),
        int2(0, 1)
    };

    for (uint i = 0u; i < 4u; ++i)
    {
        const int2 q = int2(id.xy) + offsets[i];

        if (q.x < 0 || q.y < 0 ||
            q.x >= int(g_pc.resolution) ||
            q.y >= int(g_pc.resolution))
        {
            continue;
        }

        if (Wet(q) != wet)
        {
            shoreline = true;
            break;
        }
    }

    g_wet.Store(index * 4u, wet ? 1u : 0u);
    g_shoreline.Store(index * 4u, shoreline ? 1u : 0u);
}
)";

inline constexpr const char* kM17SedimentExchangeShader = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_water : register(u0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_momentum : register(t1);
[[vk::binding(2, 0)]]
ByteAddressBuffer g_shoreline : register(t2);
[[vk::binding(3, 0)]]
ByteAddressBuffer g_geology : register(t3);
[[vk::binding(4, 0)]]
ByteAddressBuffer g_protection : register(t4);
[[vk::binding(5, 0)]]
RWByteAddressBuffer g_waterborne : register(u5);
[[vk::binding(6, 0)]]
RWByteAddressBuffer g_surfaceMobile : register(u6);

[[vk::binding(7, 0)]]
RWTexture2D<float> g_bedrock : register(u7);
[[vk::binding(8, 0)]]
RWTexture2D<float4> g_loose : register(u8);
[[vk::binding(9, 0)]]
RWTexture2D<uint> g_material : register(u9);

struct PushConstants
{
    uint resolution;
    float timeStepSeconds;
    float seaLevelMeters;
    float gravity;
    float wetThresholdMeters;
    float activeDepthMeters;
    float referenceEnergy;
    float erosionRate;
    float maximumErosionDepth;
    float sandBedloadFraction;
    float finesBedloadFraction;
    float coarseBedloadFraction;
    float regolithDensity;
    float soilDensity;
    float sandDensity;
    float debrisDensity;
    float coastalBedrockSandFraction;
};

[[vk::push_constant]] PushConstants g_pc;

float3 LoadMass(RWByteAddressBuffer buffer, uint index)
{
    const uint o = index * 16u;
    return max(float3(
        asfloat(buffer.Load(o + 0u)),
        asfloat(buffer.Load(o + 4u)),
        asfloat(buffer.Load(o + 8u))), 0.0);
}

void StoreMass(RWByteAddressBuffer buffer, uint index, float3 mass)
{
    const uint o = index * 16u;
    mass = max(mass, 0.0);
    buffer.Store(o + 0u, asuint(mass.x));
    buffer.Store(o + 4u, asuint(mass.y));
    buffer.Store(o + 8u, asuint(mass.z));
    buffer.Store(o + 12u, asuint(0.0));
}

float4 ReadRock(uint materialIndex)
{
    const uint o = materialIndex * 32u;

    return float4(
        asfloat(g_geology.Load(o + 0u)),
        asfloat(g_geology.Load(o + 4u)),
        asfloat(g_geology.Load(o + 8u)),
        asfloat(g_geology.Load(o + 28u)));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint2 p = id.xy;
    const uint index = id.y * g_pc.resolution + id.x;

    float depth = max(asfloat(g_water.Load(index * 4u)), 0.0);

    if (depth < g_pc.wetThresholdMeters ||
        depth > g_pc.activeDepthMeters)
    {
        return;
    }

    const uint mo = index * 8u;
    const float2 momentum = float2(
        asfloat(g_momentum.Load(mo + 0u)),
        asfloat(g_momentum.Load(mo + 4u)));

    const float2 velocity = momentum / max(depth, 1.0e-6);
    const float speed = length(velocity);

    float4 loose = max(g_loose[p], 0.0);
    float bedrock = g_bedrock[p];

    const float surface =
        bedrock + loose.x + loose.y + loose.z + loose.w + depth;

    const float energy =
        0.5 * speed * speed +
        g_pc.gravity * abs(surface - g_pc.seaLevelMeters);

    const float energyFactor =
        clamp(energy / max(g_pc.referenceEnergy, 1.0e-6), 0.0, 6.0);

    const float depthFactor =
        saturate(1.0 - depth / max(g_pc.activeDepthMeters, 1.0e-6));

    const float shore =
        g_shoreline.Load(index * 4u) != 0u
            ? 1.0
            : depthFactor;

    const float protection =
        saturate(asfloat(g_protection.Load(index * 4u)));

    const uint materialIndex = g_material[p];
    const float4 rock = ReadRock(materialIndex);

    const float hardness = saturate(rock.x);
    const float cohesion = saturate(rock.y);
    const float hydraulic = saturate(rock.z);
    const float bedrockDensity = max(rock.w, 1.0);

    float mobility = 0.0;

    if (loose.w > 1.0e-6)
    {
        mobility = 0.36;
    }
    else if (loose.z > 1.0e-6)
    {
        mobility = 1.0;
    }
    else if (loose.y > 1.0e-6)
    {
        mobility = 0.70;
    }
    else if (loose.x > 1.0e-6)
    {
        mobility = 0.48;
    }
    else
    {
        mobility = clamp(
            (0.15 + 0.85 * hydraulic) *
            (1.0 - 0.60 * hardness) *
            (1.0 - 0.30 * cohesion),
            0.01,
            1.0);
    }

    float remaining = min(
        g_pc.maximumErosionDepth,
        g_pc.erosionRate *
        g_pc.timeStepSeconds *
        energyFactor *
        shore *
        mobility *
        (1.0 - protection));

    if (remaining <= 0.0)
    {
        return;
    }

    const float requested = remaining;

    float3 picked = 0.0; // sand, fines, coarse [kg/m2]

    const float takeDebris = min(loose.w, remaining);
    loose.w -= takeDebris;
    remaining -= takeDebris;
    picked.z += takeDebris * g_pc.debrisDensity;

    const float takeSand = min(loose.z, remaining);
    loose.z -= takeSand;
    remaining -= takeSand;
    picked.x += takeSand * g_pc.sandDensity;

    const float takeSoil = min(loose.y, remaining);
    loose.y -= takeSoil;
    remaining -= takeSoil;
    picked.y += takeSoil * g_pc.soilDensity;

    const float takeRegolith = min(loose.x, remaining);
    loose.x -= takeRegolith;
    remaining -= takeRegolith;
    picked.y += takeRegolith * g_pc.regolithDensity;

    if (remaining > 0.0)
    {
        const float removedBedrockDepth = remaining;

        bedrock -= removedBedrockDepth;

        const float bedrockMass = removedBedrockDepth * bedrockDensity;
        const float sand = bedrockMass * saturate(g_pc.coastalBedrockSandFraction);

        picked.x += sand;
        picked.y += bedrockMass - sand;

        remaining = 0.0;
    }

    const float removedDepth = requested - remaining;

    float3 bedload = float3(
        picked.x * saturate(g_pc.sandBedloadFraction),
        picked.y * saturate(g_pc.finesBedloadFraction),
        picked.z * saturate(g_pc.coarseBedloadFraction));

    const float3 suspended = picked - bedload;

    StoreMass(
        g_waterborne,
        index,
        LoadMass(g_waterborne, index) + suspended);

    StoreMass(
        g_surfaceMobile,
        index,
        LoadMass(g_surfaceMobile, index) + bedload);

    g_bedrock[p] = bedrock;
    g_loose[p] = loose;

    depth += max(removedDepth, 0.0);
    g_water.Store(index * 4u, asuint(depth));
}
)";

inline constexpr const char* kM17SedimentTransportShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_water : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_momentum : register(t1);
[[vk::binding(2, 0)]]
ByteAddressBuffer g_waterborneIn : register(t2);
[[vk::binding(3, 0)]]
ByteAddressBuffer g_surfaceIn : register(t3);
[[vk::binding(4, 0)]]
RWByteAddressBuffer g_waterborneScratch : register(u4);
[[vk::binding(5, 0)]]
RWByteAddressBuffer g_surfaceScratch : register(u5);
[[vk::binding(6, 0)]]
RWByteAddressBuffer g_boundaryWaterborne : register(u6);
[[vk::binding(7, 0)]]
RWByteAddressBuffer g_boundarySurface : register(u7);

struct PushConstants
{
    uint resolution;
    float spacingMeters;
    float timeStepSeconds;
    float transportRate;
    float maximumTransportFraction;
    float wetThresholdMeters;
};

[[vk::push_constant]] PushConstants g_pc;

float3 LoadMass(ByteAddressBuffer buffer, uint index)
{
    const uint o = index * 16u;
    return max(float3(
        asfloat(buffer.Load(o + 0u)),
        asfloat(buffer.Load(o + 4u)),
        asfloat(buffer.Load(o + 8u))), 0.0);
}

void StoreMass(RWByteAddressBuffer buffer, uint index, float3 mass)
{
    const uint o = index * 16u;
    mass = max(mass, 0.0);
    buffer.Store(o + 0u, asuint(mass.x));
    buffer.Store(o + 4u, asuint(mass.y));
    buffer.Store(o + 8u, asuint(mass.z));
    buffer.Store(o + 12u, asuint(0.0));
}

float2 Velocity(uint2 p)
{
    const uint index = p.y * g_pc.resolution + p.x;
    const float depth = max(asfloat(g_water.Load(index * 4u)), 0.0);

    if (depth < g_pc.wetThresholdMeters)
    {
        return 0.0;
    }

    const uint mo = index * 8u;

    return float2(
        asfloat(g_momentum.Load(mo + 0u)),
        asfloat(g_momentum.Load(mo + 4u))) /
        max(depth, 1.0e-6);
}

float2 Fractions(uint2 p)
{
    const float2 velocity = Velocity(p);
    const float speed = length(velocity);

    if (speed <= 1.0e-8)
    {
        return 0.0;
    }

    const float totalFraction = clamp(
        g_pc.transportRate *
        speed *
        g_pc.timeStepSeconds /
        max(g_pc.spacingMeters, 1.0e-6),
        0.0,
        g_pc.maximumTransportFraction);

    const float sum = abs(velocity.x) + abs(velocity.y);

    if (sum <= 1.0e-8)
    {
        return 0.0;
    }

    return totalFraction * abs(velocity) / sum;
}

float3 Gather(
    ByteAddressBuffer massBuffer,
    uint2 p)
{
    const uint index = p.y * g_pc.resolution + p.x;
    const float3 own = LoadMass(massBuffer, index);
    const float2 ownFractions = Fractions(p);

    float3 result =
        own * max(1.0 - ownFractions.x - ownFractions.y, 0.0);

    if (p.x > 0u)
    {
        const uint2 q = uint2(p.x - 1u, p.y);
        const float2 velocity = Velocity(q);

        if (velocity.x > 0.0)
        {
            result += LoadMass(
                massBuffer,
                q.y * g_pc.resolution + q.x) *
                Fractions(q).x;
        }
    }

    if (p.x + 1u < g_pc.resolution)
    {
        const uint2 q = uint2(p.x + 1u, p.y);
        const float2 velocity = Velocity(q);

        if (velocity.x < 0.0)
        {
            result += LoadMass(
                massBuffer,
                q.y * g_pc.resolution + q.x) *
                Fractions(q).x;
        }
    }

    if (p.y > 0u)
    {
        const uint2 q = uint2(p.x, p.y - 1u);
        const float2 velocity = Velocity(q);

        if (velocity.y > 0.0)
        {
            result += LoadMass(
                massBuffer,
                q.y * g_pc.resolution + q.x) *
                Fractions(q).y;
        }
    }

    if (p.y + 1u < g_pc.resolution)
    {
        const uint2 q = uint2(p.x, p.y + 1u);
        const float2 velocity = Velocity(q);

        if (velocity.y < 0.0)
        {
            result += LoadMass(
                massBuffer,
                q.y * g_pc.resolution + q.x) *
                Fractions(q).y;
        }
    }

    return result;
}

void WriteBoundaryExports(
    ByteAddressBuffer sourceBuffer,
    RWByteAddressBuffer exportBuffer,
    uint2 p)
{
    const uint index = p.y * g_pc.resolution + p.x;
    const float3 source = LoadMass(sourceBuffer, index);
    const float2 velocity = Velocity(p);
    const float2 fractions = Fractions(p);

    // Layout: north, east, south, west.
    if (p.y == 0u)
    {
        StoreMass(
            exportBuffer,
            0u * g_pc.resolution + p.x,
            velocity.y < 0.0 ? source * fractions.y : 0.0);
    }

    if (p.x + 1u == g_pc.resolution)
    {
        StoreMass(
            exportBuffer,
            1u * g_pc.resolution + p.y,
            velocity.x > 0.0 ? source * fractions.x : 0.0);
    }

    if (p.y + 1u == g_pc.resolution)
    {
        StoreMass(
            exportBuffer,
            2u * g_pc.resolution + p.x,
            velocity.y > 0.0 ? source * fractions.y : 0.0);
    }

    if (p.x == 0u)
    {
        StoreMass(
            exportBuffer,
            3u * g_pc.resolution + p.y,
            velocity.x < 0.0 ? source * fractions.x : 0.0);
    }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint2 p = id.xy;
    const uint index = id.y * g_pc.resolution + id.x;

    StoreMass(
        g_waterborneScratch,
        index,
        Gather(g_waterborneIn, p));

    StoreMass(
        g_surfaceScratch,
        index,
        Gather(g_surfaceIn, p));

    WriteBoundaryExports(
        g_waterborneIn,
        g_boundaryWaterborne,
        p);

    WriteBoundaryExports(
        g_surfaceIn,
        g_boundarySurface,
        p);
}
)";

inline constexpr const char* kM17SedimentDepositShader = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_water : register(u0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_momentum : register(t1);
[[vk::binding(2, 0)]]
ByteAddressBuffer g_shoreline : register(t2);
[[vk::binding(3, 0)]]
ByteAddressBuffer g_waterborneScratch : register(t3);
[[vk::binding(4, 0)]]
ByteAddressBuffer g_surfaceScratch : register(t4);
[[vk::binding(5, 0)]]
RWByteAddressBuffer g_waterborneOut : register(u5);
[[vk::binding(6, 0)]]
RWByteAddressBuffer g_surfaceOut : register(u6);

[[vk::binding(7, 0)]]
RWTexture2D<float4> g_loose : register(u7);

struct PushConstants
{
    uint resolution;
    float timeStepSeconds;
    float wetThresholdMeters;
    float velocityThreshold;
    float depositionRate;
    float shorelineMultiplier;
    float soilDensity;
    float sandDensity;
    float debrisDensity;
};

[[vk::push_constant]] PushConstants g_pc;

float3 LoadMass(ByteAddressBuffer buffer, uint index)
{
    const uint o = index * 16u;
    return max(float3(
        asfloat(buffer.Load(o + 0u)),
        asfloat(buffer.Load(o + 4u)),
        asfloat(buffer.Load(o + 8u))), 0.0);
}

void StoreMass(RWByteAddressBuffer buffer, uint index, float3 mass)
{
    const uint o = index * 16u;
    mass = max(mass, 0.0);
    buffer.Store(o + 0u, asuint(mass.x));
    buffer.Store(o + 4u, asuint(mass.y));
    buffer.Store(o + 8u, asuint(mass.z));
    buffer.Store(o + 12u, asuint(0.0));
}

float Deposit(
    inout float3 mass,
    inout float4 loose,
    float fraction)
{
    float budget = (mass.x + mass.y + mass.z) * saturate(fraction);
    float addedDepth = 0.0;

    const float coarse = min(mass.z, budget);
    mass.z -= coarse;
    budget -= coarse;
    loose.w += coarse / max(g_pc.debrisDensity, 1.0);
    addedDepth += coarse / max(g_pc.debrisDensity, 1.0);

    const float sand = min(mass.x, budget);
    mass.x -= sand;
    budget -= sand;
    loose.z += sand / max(g_pc.sandDensity, 1.0);
    addedDepth += sand / max(g_pc.sandDensity, 1.0);

    const float fines = min(mass.y, budget);
    mass.y -= fines;
    budget -= fines;
    loose.y += fines / max(g_pc.soilDensity, 1.0);
    addedDepth += fines / max(g_pc.soilDensity, 1.0);

    return addedDepth;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = id.y * g_pc.resolution + id.x;
    float depth = max(asfloat(g_water.Load(index * 4u)), 0.0);

    const uint mo = index * 8u;
    const float2 momentum = float2(
        asfloat(g_momentum.Load(mo + 0u)),
        asfloat(g_momentum.Load(mo + 4u)));

    const float speed =
        depth >= g_pc.wetThresholdMeters
            ? length(momentum / max(depth, 1.0e-6))
            : 0.0;

    const float lowEnergy = saturate(
        1.0 -
        speed /
        max(g_pc.velocityThreshold, 1.0e-6));

    const float shoreMultiplier =
        g_shoreline.Load(index * 4u) != 0u
            ? g_pc.shorelineMultiplier
            : 1.0;

    const float surfaceFraction = saturate(
        g_pc.depositionRate *
        g_pc.timeStepSeconds *
        lowEnergy *
        shoreMultiplier);

    const float waterFraction = saturate(
        0.5 *
        g_pc.depositionRate *
        g_pc.timeStepSeconds *
        lowEnergy);

    float3 waterborne =
        LoadMass(g_waterborneScratch, index);

    float3 surface =
        LoadMass(g_surfaceScratch, index);

    float4 loose = max(g_loose[id.xy], 0.0);

    float addedDepth = 0.0;

    addedDepth += Deposit(
        surface,
        loose,
        surfaceFraction);

    addedDepth += Deposit(
        waterborne,
        loose,
        waterFraction);

    depth = max(depth - addedDepth, 0.0);

    g_loose[id.xy] = loose;
    g_water.Store(index * 4u, asuint(depth));

    StoreMass(
        g_waterborneOut,
        index,
        waterborne);

    StoreMass(
        g_surfaceOut,
        index,
        surface);
}
)";

inline constexpr const char* kM17SnapshotMaterialShader = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_materialSnapshot : register(u0);

[[vk::binding(1, 0)]]
RWTexture2D<float> g_bedrock : register(u1);
[[vk::binding(2, 0)]]
RWTexture2D<float4> g_loose : register(u2);
[[vk::binding(3, 0)]]
RWTexture2D<float2> g_moisture : register(u3);

struct PushConstants
{
    uint resolution;
};

[[vk::push_constant]] PushConstants g_pc;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.resolution || id.y >= g_pc.resolution)
    {
        return;
    }

    const uint index = id.y * g_pc.resolution + id.x;
    const uint o = index * 24u;

    const float bedrock = g_bedrock[id.xy];
    const float4 loose = g_loose[id.xy];
    const float moisture = g_moisture[id.xy].x;

    g_materialSnapshot.Store(o + 0u, asuint(bedrock));
    g_materialSnapshot.Store(o + 4u, asuint(loose.x));
    g_materialSnapshot.Store(o + 8u, asuint(loose.y));
    g_materialSnapshot.Store(o + 12u, asuint(loose.z));
    g_materialSnapshot.Store(o + 16u, asuint(loose.w));
    g_materialSnapshot.Store(o + 20u, asuint(moisture));
}
)";
} // namespace orbit::terrain_gpu::detail
