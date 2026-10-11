// GPU port of weather_lab::FastStormSolver (one level). Every entry point is one stage of FastStormSolver::Impl::Step, in the same
// order and with the same arithmetic as the CPU reference (engine/weather_lab/src/FastStormSolver.cpp); the host records them into one
// command list per batch of steps. All state lives in one storage buffer (g_a) in two ping-pong copies, so a single UAV barrier orders
// any two stages. Edit this file and call GpuStormSolver::ReloadShaders: a failed compile keeps the running pipelines.
//
// Arena layout (floats), per copy: [u: cells][v: cells][w: faceCells][scalars: nScalars * cells], scalars in the order
// theta, qv, qc, qr, qi, qs, qg (qi..qg only with ice). Outside the copies: rain accumulation (plane), the complex spectrum (2 * cells),
// the pressure potential (cells) and a small aux region (mass partial sums and ratios).

// Entry points: the host defines ENTRY_<name> as `main` for the kernel it is compiling (the RHI always uses `main`) and as an unused
// name for the others. Compiled standalone, every entry keeps its own name.
#ifndef ENTRY_AdvectVelocity
#define ENTRY_AdvectVelocity AdvectVelocity
#endif
#ifndef ENTRY_AdvectScalars
#define ENTRY_AdvectScalars AdvectScalars
#endif
#ifndef ENTRY_MassPartial
#define ENTRY_MassPartial MassPartial
#endif
#ifndef ENTRY_MassRatio
#define ENTRY_MassRatio MassRatio
#endif
#ifndef ENTRY_MassScale
#define ENTRY_MassScale MassScale
#endif
#ifndef ENTRY_Microphysics
#define ENTRY_Microphysics Microphysics
#endif
#ifndef ENTRY_Buoyancy
#define ENTRY_Buoyancy Buoyancy
#endif
#ifndef ENTRY_TopSponge
#define ENTRY_TopSponge TopSponge
#endif
#ifndef ENTRY_Mix
#define ENTRY_Mix Mix
#endif
#ifndef ENTRY_LateralSponge
#define ENTRY_LateralSponge LateralSponge
#endif
#ifndef ENTRY_ProjRhs
#define ENTRY_ProjRhs ProjRhs
#endif
#ifndef ENTRY_FftLine
#define ENTRY_FftLine FftLine
#endif
#ifndef ENTRY_ProjTridiag
#define ENTRY_ProjTridiag ProjTridiag
#endif
#ifndef ENTRY_ProjGradient
#define ENTRY_ProjGradient ProjGradient
#endif

static const float kGravity = 9.81;
static const float kCp = 1005.7;
static const float kRv = 461.5;
static const float kLv = 2.5e6;
static const float kLs = 2.834e6;
static const float kLf = kLs - kLv;
static const float kPi = 3.14159265358979;

struct Params
{
    uint nx, ny, nz, plane;
    uint cells, faceCells, flags, nScalars;   // flags: 1 cubic advection, 2 moisture, 4 ice
    float dx, dy, dz, dt;
    float frameU, frameV, ch, cv;
    uint offU, offV, offW, offS;              // current copy
    uint dstU, dstV, dstW, dstS;              // the other copy
    uint offRain, offSpec, offPhi, offAux;
    uint arg0, arg1, arg2, arg3;
    float spongeBase, spongeRate, latCells, latRate;
};

[[vk::push_constant]] Params P;
[[vk::binding(0, 0)]] RWStructuredBuffer<float> g_a;
[[vk::binding(1, 0)]] StructuredBuffer<float> g_base;

// ----------------------------------------------------------------------------- base state
// g_base layout: theta, vapor, exner, pressure, density, windU, windV (nz each), faceDensity (nz + 1), lower, upper (nz each),
// lambdaX (nx), lambdaY (ny).
float BTheta(uint k)    { return g_base[k]; }
float BVapor(uint k)    { return g_base[P.nz + k]; }
float BExner(uint k)    { return g_base[2u * P.nz + k]; }
float BPressure(uint k) { return g_base[3u * P.nz + k]; }
float BDensity(uint k)  { return g_base[4u * P.nz + k]; }
float BWindU(uint k)    { return g_base[5u * P.nz + k]; }
float BWindV(uint k)    { return g_base[6u * P.nz + k]; }
float BFace(uint k)     { return g_base[7u * P.nz + k]; }
float BLower(uint k)    { return g_base[8u * P.nz + 1u + k]; }
float BUpper(uint k)    { return g_base[9u * P.nz + 1u + k]; }
float BLambdaX(uint m)  { return g_base[10u * P.nz + 1u + m]; }
float BLambdaY(uint m)  { return g_base[10u * P.nz + 1u + P.nx + m]; }

bool Moist() { return (P.flags & 2u) != 0u; }
bool Ice()   { return (P.flags & 4u) != 0u; }

float S(uint s, uint idx)            { return g_a[P.offS + s * P.cells + idx]; }
void  SetS(uint s, uint idx, float v) { g_a[P.offS + s * P.cells + idx] = v; }

float SaturationMixingRatio(float p, float t)
{
    const float es = 611.2 * exp(17.67 * (t - 273.15) / (t - 29.65));
    return 0.622 * es / max(p - es, 1.0);
}

float SaturationMixingRatioIce(float p, float t)
{
    const float es = 611.2 * exp(21.875 * (t - 273.15) / (t - 7.66));
    return 0.622 * es / max(p - es, 1.0);
}

// ----------------------------------------------------------------------------- sampling
float Tri(uint off, int levels, float ix, float iy, float iz)
{
    const float fx = floor(ix), fy = floor(iy), fz = floor(iz);
    const int i0 = (int)fx, j0 = (int)fy, k0 = (int)fz;
    const float tx = ix - fx, ty = iy - fy, tz = iz - fz;
    const int mx = (int)P.nx - 1, my = (int)P.ny - 1;
    const int ia = i0 & mx, ib = (i0 + 1) & mx;
    float plane[2];
    [unroll] for (int dk = 0; dk < 2; ++dk)
    {
        const int k = clamp(k0 + dk, 0, levels - 1);
        const uint r0 = off + ((uint)k * P.ny + (uint)(j0 & my)) * P.nx;
        const uint r1 = off + ((uint)k * P.ny + (uint)((j0 + 1) & my)) * P.nx;
        const float a = lerp(g_a[r0 + ia], g_a[r0 + ib], tx);
        const float b = lerp(g_a[r1 + ia], g_a[r1 + ib], tx);
        plane[dk] = lerp(a, b, ty);
    }
    return lerp(plane[0], plane[1], tz);
}

float CatmullRom(float p0, float p1, float p2, float p3, float t)
{
    return p1 + 0.5 * t * (p2 - p0 + t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 + t * (3.0 * (p1 - p2) + p3 - p0)));
}

float MonotoneCubic(uint off, int levels, float ix, float iy, float iz)
{
    const float fx = floor(ix), fy = floor(iy), fz = floor(iz);
    const int i0 = (int)fx, j0 = (int)fy, k0 = (int)fz;
    const float tx = ix - fx, ty = iy - fy, tz = iz - fz;
    const int mx = (int)P.nx - 1, my = (int)P.ny - 1;
    int xi[4];
    [unroll] for (int d = 0; d < 4; ++d) { xi[d] = (i0 + d - 1) & mx; }
    float lo = 3.4e38, hi = -3.4e38;
    float alongZ[4];
    [unroll] for (int dk = 0; dk < 4; ++dk)
    {
        const int k = clamp(k0 + dk - 1, 0, levels - 1);
        float alongY[4];
        [unroll] for (int dj = 0; dj < 4; ++dj)
        {
            const uint r = off + ((uint)k * P.ny + (uint)((j0 + dj - 1) & my)) * P.nx;
            const float a = g_a[r + xi[0]], b = g_a[r + xi[1]], c = g_a[r + xi[2]], d2 = g_a[r + xi[3]];
            alongY[dj] = CatmullRom(a, b, c, d2, tx);
            if (dk >= 1 && dk <= 2 && dj >= 1 && dj <= 2)
            {
                lo = min(lo, min(b, c));
                hi = max(hi, max(b, c));
            }
        }
        alongZ[dk] = CatmullRom(alongY[0], alongY[1], alongY[2], alongY[3], ty);
    }
    const float value = CatmullRom(alongZ[0], alongZ[1], alongZ[2], alongZ[3], tz);
    return min(max(value, lo), hi);
}

void Velocity(float x, float y, float z, out float ox, out float oy, out float oz)
{
    const float invDx = 1.0 / P.dx, invDy = 1.0 / P.dy, invDz = 1.0 / P.dz;
    ox = Tri(P.offU, (int)P.nz, x * invDx, y * invDy - 0.5, z * invDz - 0.5) - P.frameU;
    oy = Tri(P.offV, (int)P.nz, x * invDx - 0.5, y * invDy, z * invDz - 0.5) - P.frameV;
    oz = Tri(P.offW, (int)P.nz + 1, x * invDx - 0.5, y * invDy - 0.5, z * invDz);
}

// Semi-Lagrangian departure point (RK2) of the sample at (x, y, z), returned as fractional indices of the staggering (sx, sy, sz).
void Departure(float x, float y, float z, float sx, float sy, float sz, out float ix, out float iy, out float iz)
{
    const float zTop = (float)P.nz * P.dz;
    float vx, vy, vz;
    Velocity(x, y, z, vx, vy, vz);
    const float mx = x - 0.5 * P.dt * vx;
    const float my = y - 0.5 * P.dt * vy;
    const float mz = min(max(z - 0.5 * P.dt * vz, 0.0), zTop);
    Velocity(mx, my, mz, vx, vy, vz);
    const float dxp = x - P.dt * vx;
    const float dyp = y - P.dt * vy;
    const float dzp = min(max(z - P.dt * vz, 0.0), zTop);
    ix = dxp / P.dx - sx;
    iy = dyp / P.dy - sy;
    iz = dzp / P.dz - sz;
}

float SampleAt(uint off, int levels, float ix, float iy, float iz)
{
    return (P.flags & 1u) != 0u ? MonotoneCubic(off, levels, ix, iy, iz) : Tri(off, levels, ix, iy, iz);
}

// ----------------------------------------------------------------------------- advection
// arg0: 0 u, 1 v, 2 w. Dispatch (nx/8, ny/8, levels).
[numthreads(8, 8, 1)]
void ENTRY_AdvectVelocity(uint3 id : SV_DispatchThreadID)
{
    const uint mode = P.arg0;
    const uint levels = mode == 2u ? P.nz + 1u : P.nz;
    if (id.x >= P.nx || id.y >= P.ny || id.z >= levels) { return; }
    const uint k = id.z;
    const uint idx = (k * P.ny + id.y) * P.nx + id.x;
    if (mode == 2u && (k == 0u || k == P.nz))
    {
        g_a[P.dstW + idx] = 0.0;   // rigid floor and lid
        return;
    }
    const float sx = mode == 0u ? 0.0 : 0.5;
    const float sy = mode == 1u ? 0.0 : 0.5;
    const float sz = mode == 2u ? 0.0 : 0.5;
    float ix, iy, iz;
    Departure((id.x + sx) * P.dx, (id.y + sy) * P.dy, (k + sz) * P.dz, sx, sy, sz, ix, iy, iz);
    const uint src = mode == 0u ? P.offU : (mode == 1u ? P.offV : P.offW);
    const uint dst = mode == 0u ? P.dstU : (mode == 1u ? P.dstV : P.dstW);
    g_a[dst + idx] = SampleAt(src, (int)levels, ix, iy, iz);
}

// All scalar fields share one trace. Dispatch (nx/8, ny/8, nz).
[numthreads(8, 8, 1)]
void ENTRY_AdvectScalars(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.nx || id.y >= P.ny || id.z >= P.nz) { return; }
    const uint idx = (id.z * P.ny + id.y) * P.nx + id.x;
    float ix, iy, iz;
    Departure((id.x + 0.5) * P.dx, (id.y + 0.5) * P.dy, (id.z + 0.5) * P.dz, 0.5, 0.5, 0.5, ix, iy, iz);
    for (uint s = 0u; s < P.nScalars; ++s)
    {
        g_a[P.dstS + s * P.cells + idx] = SampleAt(P.offS + s * P.cells, (int)P.nz, ix, iy, iz);
    }
}

// ----------------------------------------------------------------------------- mass fixer
groupshared float gs_sum[256];

// Dispatch (nz, 2 * (nScalars - 1)): group y = 2 * (species - 1) + which (0 = before advection = current copy, 1 = after).
[numthreads(256, 1, 1)]
void ENTRY_MassPartial(uint3 group : SV_GroupID, uint t : SV_GroupIndex)
{
    const uint k = group.x;
    const uint sp = group.y / 2u + 1u;
    const uint which = group.y % 2u;
    const uint base = (which == 0u ? P.offS : P.dstS) + sp * P.cells + k * P.plane;
    float sum = 0.0;
    for (uint c = t; c < P.plane; c += 256u) { sum += g_a[base + c]; }
    gs_sum[t] = sum;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128u; stride > 0u; stride >>= 1u)
    {
        if (t < stride) { gs_sum[t] += gs_sum[t + stride]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if (t == 0u) { g_a[P.offAux + group.y * P.nz + k] = gs_sum[0]; }
}

// Dispatch (1): one thread per species. Ratio of the mass before to the mass after, weighted by density.
[numthreads(8, 1, 1)]
void ENTRY_MassRatio(uint3 id : SV_DispatchThreadID)
{
    const uint sp = id.x + 1u;
    if (sp >= P.nScalars) { return; }
    float m0 = 0.0, m1 = 0.0;
    for (uint k = 0u; k < P.nz; ++k)
    {
        const float rho = BDensity(k);
        m0 += g_a[P.offAux + (2u * (sp - 1u)) * P.nz + k] * rho;
        m1 += g_a[P.offAux + (2u * (sp - 1u) + 1u) * P.nz + k] * rho;
    }
    g_a[P.offAux + 2u * (P.nScalars - 1u) * P.nz + sp] = (m0 > 0.0 && m1 > 0.0) ? m0 / m1 : 1.0;
}

[numthreads(8, 8, 1)]
void ENTRY_MassScale(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.nx || id.y >= P.ny || id.z >= P.nz) { return; }
    const uint idx = (id.z * P.ny + id.y) * P.nx + id.x;
    for (uint sp = 1u; sp < P.nScalars; ++sp)
    {
        g_a[P.dstS + sp * P.cells + idx] *= g_a[P.offAux + 2u * (P.nScalars - 1u) * P.nz + sp];
    }
}

// ----------------------------------------------------------------------------- microphysics
void IceProcesses(float dt, float p, float exner, inout float th, inout float vapor, inout float cloud, inout float rain,
    inout float ice, inout float snow, inout float graupel)
{
    const float heatF = kLf / (kCp * exner);
    const float heatS = kLs / (kCp * exner);
    float temperature = th * exner;
    const float kFreeze = 273.15;

    if (temperature < 233.15)
    {
        ice += cloud;
        graupel += rain;
        th += heatF * (cloud + rain);
        cloud = 0.0;
        rain = 0.0;
        temperature = th * exner;
    }
    const float supercool = kFreeze - temperature;

    if (supercool > 5.0)
    {
        const float tauCloud = clamp(2000.0 * exp(-0.35 * (supercool - 5.0)), 20.0, 2000.0);
        const float frozenCloud = cloud * (1.0 - exp(-dt / tauCloud));
        const float tauRain = clamp(120.0 * exp(-0.3 * (supercool - 5.0)), 10.0, 120.0);
        const float frozenRain = rain * (1.0 - exp(-dt / tauRain));
        cloud -= frozenCloud;
        ice += frozenCloud;
        rain -= frozenRain;
        graupel += frozenRain;
        th += heatF * (frozenCloud + frozenRain);
        temperature = th * exner;
    }

    if (temperature < kFreeze)
    {
        const float qvi = SaturationMixingRatioIce(p, temperature);
        if (ice + snow + graupel < 1.0e-8 && temperature < 263.15 && vapor > 1.05 * qvi)
        {
            ice += 1.0e-7;
            vapor -= 1.0e-7;
            th += heatS * 1.0e-7;
        }
        const float totalIce = ice + snow + graupel;
        if (totalIce > 0.0)
        {
            const float denominator = 1.0 + kLs * kLs * qvi / (kCp * kRv * temperature * temperature);
            const float excess = (vapor - qvi) / denominator;
            const float rate = min(1.0, dt / 300.0);
            if (excess > 0.0)
            {
                const float deposited = min(excess * rate, vapor);
                const float shareIce = ice / totalIce;
                ice += deposited * max(shareIce, 0.5);
                snow += deposited * (1.0 - max(shareIce, 0.5));
                vapor -= deposited;
                th += heatS * deposited;
            }
            else
            {
                const float wanted = min(-excess * min(1.0, dt / 900.0), totalIce);
                const float fromGraupel = min(graupel, wanted * graupel / totalIce);
                const float fromSnow = min(snow, wanted * snow / totalIce);
                const float fromIce = min(ice, wanted - fromGraupel - fromSnow);
                graupel -= fromGraupel;
                snow -= fromSnow;
                ice -= fromIce;
                const float taken = fromGraupel + fromSnow + fromIce;
                vapor += taken;
                th -= heatS * taken;
            }
            temperature = th * exner;
        }

        const float toSnow = min(dt * 1.0e-3 * max(ice - 1.0e-4, 0.0), ice);
        ice -= toSnow;
        snow += toSnow;

        const float rimeSnow = min(dt * 1.0 * cloud * pow(max(snow, 0.0), 0.875), cloud);
        cloud -= rimeSnow;
        snow += rimeSnow;
        const float rimeGraupel = min(dt * 3.0 * cloud * pow(max(graupel, 0.0), 0.875), cloud);
        cloud -= rimeGraupel;
        graupel += rimeGraupel;
        const float collectedRain = min(dt * 0.5 * rain * pow(max(snow + graupel, 0.0), 0.875), rain);
        rain -= collectedRain;
        graupel += collectedRain;
        th += heatF * (rimeSnow + rimeGraupel + collectedRain);
        const float toGraupel = min(dt * 1.0e-3 * max(snow - 2.0e-3, 0.0) * (cloud > 5.0e-4 ? 1.0 : 0.2), snow);
        snow -= toGraupel;
        graupel += toGraupel;
    }
    else if (ice + snow + graupel > 0.0)
    {
        const float excessHeat = (temperature - kFreeze) / heatF;
        float budget = excessHeat;
        const float meltIce = min(ice, budget);
        ice -= meltIce;
        cloud += meltIce;
        budget -= meltIce;
        const float meltSnow = min(snow, min(budget, snow * min(1.0, dt / 60.0) + 1.0e-9));
        snow -= meltSnow;
        rain += meltSnow;
        budget -= meltSnow;
        const float meltGraupel = min(graupel, min(budget, graupel * min(1.0, dt / 120.0) + 1.0e-9));
        graupel -= meltGraupel;
        rain += meltGraupel;
        th -= heatF * (meltIce + meltSnow + meltGraupel);
    }
}

// Fall speed of one species: 0 rain, 1 cloud ice, 2 snow, 3 graupel.
float FallSpeed(uint kind, uint k, float q)
{
    const float rho = BDensity(k);
    const float r = rho * max(q, 0.0);
    const float density = sqrt(BDensity(0u) / rho);
    if (kind == 0u) { return r > 0.0 ? 14.34 * pow(r, 0.1346) * density : 0.0; }
    if (kind == 1u) { return q > 0.0 ? 0.3 * density : 0.0; }
    if (kind == 2u) { return r > 0.0 ? min(1.1 * pow(r * 1000.0, 0.11), 2.0) * density : 0.0; }
    return r > 0.0 ? min(5.0 * pow(r * 1000.0, 0.2), 12.0) * density : 0.0;
}

static float gFlux[129];

void Sediment(uint scalar, uint kind, uint col)
{
    const uint field = P.offS + scalar * P.cells;
    float maxFall = 0.0;
    for (uint k = 0u; k < P.nz; ++k) { maxFall = max(maxFall, FallSpeed(kind, k, g_a[field + k * P.plane + col])); }
    if (maxFall <= 0.0) { return; }
    const uint substeps = (uint)ceil(maxFall * P.dt / (0.9 * P.dz));
    const float sub = P.dt / (float)substeps;
    for (uint s = 0u; s < substeps; ++s)
    {
        for (uint k = 0u; k < P.nz; ++k)
        {
            const float q = g_a[field + k * P.plane + col];
            gFlux[k] = BDensity(k) * q * FallSpeed(kind, k, q);
        }
        gFlux[P.nz] = 0.0;
        for (uint k2 = 0u; k2 < P.nz; ++k2)
        {
            const uint idx = field + k2 * P.plane + col;
            g_a[idx] = max(g_a[idx] + sub * (gFlux[k2 + 1u] - gFlux[k2]) / (BDensity(k2) * P.dz), 0.0);
        }
        g_a[P.offRain + col] += sub * gFlux[0];
    }
}

// One thread per column. Dispatch (plane / 64).
[numthreads(64, 1, 1)]
void ENTRY_Microphysics(uint3 id : SV_DispatchThreadID)
{
    const uint col = id.x;
    if (col >= P.plane) { return; }
    const bool ice = Ice();
    for (uint k = 0u; k < P.nz; ++k)
    {
        const uint idx = k * P.plane + col;
        const float p = BPressure(k);
        const float exner = BExner(k);
        const float rho = BDensity(k);
        float th = S(0u, idx);
        float vapor = S(1u, idx);
        float cloud = S(2u, idx);
        float rain = S(3u, idx);
        float iceQ = ice ? S(4u, idx) : 0.0;
        float snow = ice ? S(5u, idx) : 0.0;
        float graupel = ice ? S(6u, idx) : 0.0;
        const float heat = kLv / (kCp * exner);

        if (cloud <= 0.0 && rain <= 0.0 && iceQ <= 0.0 && snow <= 0.0 && graupel <= 0.0
            && vapor <= 0.98 * SaturationMixingRatio(p, th * exner))
        {
            continue;
        }

        [unroll] for (int iteration = 0; iteration < 2; ++iteration)
        {
            const float temperature = th * exner;
            const float qvs = SaturationMixingRatio(p, temperature);
            const float denominator = 1.0 + kLv * kLv * qvs / (kCp * kRv * temperature * temperature);
            float change = (vapor - qvs) / denominator;
            change = change >= 0.0 ? min(change, vapor) : -min(-change, cloud);
            vapor -= change;
            cloud += change;
            th += heat * change;
        }

        const float autoc = min(P.dt * 1.0e-3 * max(cloud - 1.0e-3, 0.0), cloud);
        const float accretion = min(P.dt * 2.2 * cloud * pow(max(rain, 0.0), 0.875), cloud - autoc);
        cloud -= autoc + accretion;
        rain += autoc + accretion;

        if (rain > 0.0)
        {
            const float temperature = th * exner;
            const float qvs = SaturationMixingRatio(p, temperature);
            if (vapor < qvs)
            {
                const float rhoQr = rho * rain;
                const float evap = P.dt * (1.6 + 30.3922 * pow(rhoQr, 0.2046)) * (1.0 - vapor / qvs) * pow(rhoQr, 0.525)
                    / ((2.03e4 + 9.584e6 / (qvs * p)) * rho);
                const float denominator = 1.0 + kLv * kLv * qvs / (kCp * kRv * temperature * temperature);
                const float taken = min(min(evap, rain), (qvs - vapor) / denominator);
                rain -= taken;
                vapor += taken;
                th -= heat * taken;
            }
        }
        if (ice)
        {
            IceProcesses(P.dt, p, exner, th, vapor, cloud, rain, iceQ, snow, graupel);
            SetS(4u, idx, iceQ);
            SetS(5u, idx, snow);
            SetS(6u, idx, graupel);
        }
        SetS(0u, idx, th);
        SetS(1u, idx, vapor);
        SetS(2u, idx, cloud);
        SetS(3u, idx, rain);
    }
    Sediment(3u, 0u, col);
    if (ice)
    {
        Sediment(4u, 1u, col);
        Sediment(5u, 2u, col);
        Sediment(6u, 3u, col);
    }
}

// ----------------------------------------------------------------------------- forcing
// Buoyancy on the interior w faces. Dispatch (nx/8, ny/8, nz - 1).
[numthreads(8, 8, 1)]
void ENTRY_Buoyancy(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.nx || id.y >= P.ny || id.z + 1u >= P.nz) { return; }
    const uint k = id.z + 1u;
    const uint c = id.y * P.nx + id.x;
    float b[2];
    [unroll] for (uint side = 0u; side < 2u; ++side)
    {
        const uint level = k - 1u + side;
        const uint idx = level * P.plane + c;
        float load = 0.0;
        float vapor = 0.0;
        if (Moist())
        {
            load = S(2u, idx) + S(3u, idx);
            if (Ice()) { load += S(4u, idx) + S(5u, idx) + S(6u, idx); }
            vapor = S(1u, idx) - BVapor(level);
        }
        b[side] = kGravity * ((S(0u, idx) - BTheta(level)) / BTheta(level) + 0.608 * vapor - load);
    }
    g_a[P.offW + k * P.plane + c] += P.dt * 0.5 * (b[0] + b[1]);
}

// Top sponge on w (faces 0..nz) and theta (cells). Dispatch (nx/8, ny/8, nz + 1).
[numthreads(8, 8, 1)]
void ENTRY_TopSponge(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.nx || id.y >= P.ny || id.z > P.nz) { return; }
    const float top = (float)P.nz * P.dz;
    if (P.spongeBase >= top) { return; }
    const uint k = id.z;
    const float z = (float)k * P.dz;
    if (z <= P.spongeBase) { return; }
    const float s = sin(0.5 * kPi * (z - P.spongeBase) / (top - P.spongeBase));
    const float damp = 1.0 / (1.0 + P.dt * P.spongeRate * s * s);
    const uint c = id.y * P.nx + id.x;
    g_a[P.offW + k * P.plane + c] *= damp;
    if (k < P.nz)
    {
        const uint idx = k * P.plane + c;
        const float base = BTheta(k);
        SetS(0u, idx, base + (S(0u, idx) - base) * damp);
    }
}

float MixBase(uint kind, uint k)
{
    if (kind == 1u) { return BTheta(k); }
    if (kind == 2u) { return BVapor(k); }
    if (kind == 3u) { return BWindU(k); }
    if (kind == 4u) { return BWindV(k); }
    return 0.0;
}

float MixAt(uint src, int i, int j, uint k, uint kind)
{
    const int mx = (int)P.nx - 1, my = (int)P.ny - 1;
    return g_a[src + (k * P.ny + (uint)(j & my)) * P.nx + (uint)(i & mx)] - MixBase(kind, k);
}

// Explicit mixing of one field, out of place into the other copy. arg0: source offset, arg1: destination offset,
// arg2: base kind (0 none, 1 theta, 2 vapour, 3 wind u, 4 wind v), arg3: 1 for w (nz + 1 levels, interior only).
// Dispatch (nx/8, ny/8, levels).
[numthreads(8, 8, 1)]
void ENTRY_Mix(uint3 id : SV_DispatchThreadID)
{
    const bool isW = P.arg3 == 1u;
    const uint levels = isW ? P.nz + 1u : P.nz;
    if (id.x >= P.nx || id.y >= P.ny || id.z >= levels) { return; }
    const uint k = id.z;
    const uint idx = (k * P.ny + id.y) * P.nx + id.x;
    const float value = g_a[P.arg0 + idx];
    if (isW && (k == 0u || k == P.nz))
    {
        g_a[P.arg1 + idx] = value;
        return;
    }
    const int i = (int)id.x, j = (int)id.y;
    const float c = MixAt(P.arg0, i, j, k, P.arg2);
    float tendency = P.ch * (MixAt(P.arg0, i + 1, j, k, P.arg2) + MixAt(P.arg0, i - 1, j, k, P.arg2)
        + MixAt(P.arg0, i, j + 1, k, P.arg2) + MixAt(P.arg0, i, j - 1, k, P.arg2) - 4.0 * c);
    const float below = k > 0u ? MixAt(P.arg0, i, j, k - 1u, P.arg2) : c;
    const float above = (k + 1u) < levels ? MixAt(P.arg0, i, j, k + 1u, P.arg2) : c;
    tendency += P.cv * (below + above - 2.0 * c);
    g_a[P.arg1 + idx] = value + tendency;
}

// Lateral sponge, in place. Dispatch (nx/8, ny/8, nz).
[numthreads(8, 8, 1)]
void ENTRY_LateralSponge(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.nx || id.y >= P.ny || id.z >= P.nz) { return; }
    const int width = (int)P.latCells;
    if (width <= 0) { return; }
    const int i = (int)id.x, j = (int)id.y;
    const int d = min(min(i, (int)P.nx - 1 - i), min(j, (int)P.ny - 1 - j));
    if (d >= width) { return; }
    const float ramp = cos(0.5 * kPi * (float)d / (float)width);
    const float a = 1.0 - exp(-P.dt * P.latRate * ramp * ramp);
    const uint k = id.z;
    const uint idx = (k * P.ny + id.y) * P.nx + id.x;
    g_a[P.offU + idx] += a * (BWindU(k) - g_a[P.offU + idx]);
    g_a[P.offV + idx] += a * (BWindV(k) - g_a[P.offV + idx]);
    SetS(0u, idx, S(0u, idx) + a * (BTheta(k) - S(0u, idx)));
    if (k > 0u) { g_a[P.offW + idx] -= a * g_a[P.offW + idx]; }
    if (Moist())
    {
        SetS(1u, idx, S(1u, idx) + a * (BVapor(k) - S(1u, idx)));
        SetS(2u, idx, S(2u, idx) - a * S(2u, idx));
        SetS(3u, idx, S(3u, idx) - a * S(3u, idx));
        if (Ice())
        {
            SetS(4u, idx, S(4u, idx) - a * S(4u, idx));
            SetS(5u, idx, S(5u, idx) - a * S(5u, idx));
            SetS(6u, idx, S(6u, idx) - a * S(6u, idx));
        }
    }
}

// ----------------------------------------------------------------------------- pressure projection
// Right-hand side: divergence of the density-weighted velocity / dt, as a complex field. Dispatch (nx/8, ny/8, nz).
[numthreads(8, 8, 1)]
void ENTRY_ProjRhs(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.nx || id.y >= P.ny || id.z >= P.nz) { return; }
    const uint k = id.z;
    const uint idx = (k * P.ny + id.y) * P.nx + id.x;
    const uint ip = (id.x + 1u) & (P.nx - 1u);
    const uint jp = (id.y + 1u) & (P.ny - 1u);
    const float rc = BDensity(k);
    const float div = (g_a[P.offU + (k * P.ny + id.y) * P.nx + ip] - g_a[P.offU + idx]) / P.dx
        + (g_a[P.offV + (k * P.ny + jp) * P.nx + id.x] - g_a[P.offV + idx]) / P.dy
        + (BFace(k + 1u) * g_a[P.offW + idx + P.plane] - BFace(k) * g_a[P.offW + idx]) / rc / P.dz;
    g_a[P.offSpec + 2u * idx] = div / P.dt;
    g_a[P.offSpec + 2u * idx + 1u] = 0.0;
}

groupshared float2 gs_fft[256];

float2 CMul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

// One FFT line per group (n <= 256). arg0: axis (0 x, 1 y), arg1: inverse, arg2: write the real part to phi (scaled by 1/plane).
// Dispatch (axis 0: ny, nz; axis 1: nx, nz).
[numthreads(128, 1, 1)]
void ENTRY_FftLine(uint3 group : SV_GroupID, uint t : SV_GroupIndex)
{
    const bool axisY = P.arg0 == 1u;
    const bool inverse = P.arg1 == 1u;
    const uint n = axisY ? P.ny : P.nx;
    const uint k = group.y;
    const uint lineIndex = group.x;
    const uint bits = firstbithigh(n);
    for (uint e = t; e < n; e += 128u)
    {
        const uint src = axisY ? (k * P.ny + e) * P.nx + lineIndex : (k * P.ny + lineIndex) * P.nx + e;
        gs_fft[reversebits(e) >> (32u - bits)] = float2(g_a[P.offSpec + 2u * src], g_a[P.offSpec + 2u * src + 1u]);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint len = 2u; len <= n; len <<= 1u)
    {
        const uint halfLen = len >> 1u;
        for (uint tt = t; tt < n / 2u; tt += 128u)
        {
            const uint grp = tt / halfLen;
            const uint pos = tt % halfLen;
            const uint start = grp * len;
            const float angle = (inverse ? 2.0 : -2.0) * kPi * (float)pos / (float)len;
            const float2 w = float2(cos(angle), sin(angle));
            const float2 a = gs_fft[start + pos];
            const float2 b = CMul(gs_fft[start + pos + halfLen], w);
            gs_fft[start + pos] = a + b;
            gs_fft[start + pos + halfLen] = a - b;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    for (uint e2 = t; e2 < n; e2 += 128u)
    {
        const uint dst = axisY ? (k * P.ny + e2) * P.nx + lineIndex : (k * P.ny + lineIndex) * P.nx + e2;
        if (P.arg2 == 1u)
        {
            g_a[P.offPhi + dst] = gs_fft[e2].x / (float)P.plane;
        }
        else
        {
            g_a[P.offSpec + 2u * dst] = gs_fft[e2].x;
            g_a[P.offSpec + 2u * dst + 1u] = gs_fft[e2].y;
        }
    }
}

static float2 gDPrime[128];
static float gCPrime[128];

// Vertical tridiagonal solve per horizontal wavenumber. Dispatch (plane / 64).
[numthreads(64, 1, 1)]
void ENTRY_ProjTridiag(uint3 id : SV_DispatchThreadID)
{
    const uint mode = id.x;
    if (mode >= P.plane) { return; }
    const float lambda = BLambdaX(mode % P.nx) + BLambdaY(mode / P.nx);
    const bool singular = mode == 0u;
    const uint base = P.offSpec + 2u * mode;
    const uint stride = 2u * P.plane;
    float centre0 = -BLower(0u) - BUpper(0u) - lambda;
    float2 rhs0 = float2(g_a[base], g_a[base + 1u]);
    if (singular)
    {
        centre0 = 1.0;
        rhs0 = float2(0.0, 0.0);
    }
    const float upper0 = singular ? 0.0 : BUpper(0u);
    gCPrime[0] = upper0 / centre0;
    gDPrime[0] = rhs0 / centre0;
    for (uint k = 1u; k < P.nz; ++k)
    {
        const float centre = -BLower(k) - BUpper(k) - lambda;
        const float denom = centre - BLower(k) * gCPrime[k - 1u];
        gCPrime[k] = BUpper(k) / denom;
        const float2 rhs = float2(g_a[base + k * stride], g_a[base + k * stride + 1u]);
        gDPrime[k] = (rhs - BLower(k) * gDPrime[k - 1u]) / denom;
    }
    float2 next = gDPrime[P.nz - 1u];
    g_a[base + (P.nz - 1u) * stride] = next.x;
    g_a[base + (P.nz - 1u) * stride + 1u] = next.y;
    for (uint kk = P.nz - 1u; kk-- > 0u;)
    {
        next = gDPrime[kk] - gCPrime[kk] * next;
        g_a[base + kk * stride] = next.x;
        g_a[base + kk * stride + 1u] = next.y;
    }
}

// Subtract the pressure gradient. Dispatch (nx/8, ny/8, nz).
[numthreads(8, 8, 1)]
void ENTRY_ProjGradient(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P.nx || id.y >= P.ny || id.z >= P.nz) { return; }
    const uint k = id.z;
    const uint idx = (k * P.ny + id.y) * P.nx + id.x;
    const uint im = (id.x + P.nx - 1u) & (P.nx - 1u);
    const uint jm = (id.y + P.ny - 1u) & (P.ny - 1u);
    const float phi = g_a[P.offPhi + idx];
    g_a[P.offU + idx] -= P.dt / P.dx * (phi - g_a[P.offPhi + (k * P.ny + id.y) * P.nx + im]);
    g_a[P.offV + idx] -= P.dt / P.dy * (phi - g_a[P.offPhi + (k * P.ny + jm) * P.nx + id.x]);
    if (k > 0u)
    {
        g_a[P.offW + idx] -= P.dt / P.dz * (phi - g_a[P.offPhi + idx - P.plane]);
    }
}
