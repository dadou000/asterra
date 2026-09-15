#pragma once
namespace orbit::terrain_gpu::detail
{
// GPU port of engine/terrain's CPU procedural stack: ProceduralNoise.hpp,
// TectonicField.cpp, GlobalTerrainFields.cpp, AnalyticTerrainSource.cpp,
// and TerrainFields.cpp's ClassifyBiomeWeights, in that dependency order.
//
// - Hashing (Mix64/HashLattice/MakeNoiseCell below) is a bit-exact port
//   of ProceduralNoise.hpp's real 64-bit hash, using HLSL uint64_t under
//   the Vulkan shaderInt64 feature (enabled in VulkanDevice.cpp). This
//   is deliberate, and different from the GPU-only hydrology relaxation
//   passes elsewhere in this milestone (which are free to use a
//   different parallel algorithm entirely): the CPU-generated 2D map
//   and hydrology sample this exact same elevation/climate field, so it
//   has to actually match, or GPU-rendered terrain would show different
//   coastlines/mountains than the CPU-rendered map claims are there.
// - Every noise/domain-warp coordinate is plain float (not double).
//   Continental/climate/macro/coarse-mountain bands stay accurate at
//   this precision (their direction*frequency products stay under
//   ~300 in magnitude); only the finest detail/mountain octaves
//   (frequency up to ~1e5) lose meaningful precision -- and those are
//   also the most amplitude-attenuated octaves in the fBm sum, so the
//   resulting warp is sub-visual in practice.
// - Tectonic plate seeds and hotspot chains are NOT regenerated here --
//   they're uploaded verbatim from the CPU GlobalTerrainFields instance
//   this generator was built from (see GpuFieldGenerator.cpp), so the
//   GPU-rendered clipmap terrain still shows the exact same mountain
//   ranges/islands as anything still generated CPU-side (the 2D map,
//   hydrology).
inline constexpr const char* kFieldGenerationComputeShader = R"(
// === Static parameter buffer layout (see GpuFieldGenerator.cpp's
// BuildParamsBuffer -- the indices below MUST match it exactly) ===
static const uint kParamPlanetRadiusMeters = 0;
static const uint kParamSeaLevelMeters = 1;
static const uint kParamMaximumElevationAboveSeaLevelMeters = 2;
static const uint kParamMacroAmplitudeMeters = 3;
static const uint kParamMacroWavelengthMeters = 4;
static const uint kParamDetailAmplitudeMeters = 5;
static const uint kParamDetailWavelengthMeters = 6;
static const uint kParamDetailOctaves = 7;
static const uint kParamMountainReliefMeters = 8;
static const uint kParamMountainWavelengthMeters = 9;
static const uint kParamMountainOctaves = 10;
static const uint kParamWarpWavelengthMeters = 11;
static const uint kParamWarpAmplitudeMeters = 12;
static const uint kParamWarpFrequency = 13;
static const uint kParamWarpFootprintScale = 14;
static const uint kParamMountainNormalization = 15;
static const uint kParamContinentalAmplitudeMeters = 16;
static const uint kParamContinentalWavelengthMeters = 17;
static const uint kParamContinentalBiasMeters = 18;
static const uint kParamTectonicContinentInfluence = 19;
static const uint kParamGlobalMountainAmplitudeMeters = 20;
static const uint kParamGlobalMountainWavelengthMeters = 21;
static const uint kParamClimateWavelengthMeters = 22;
static const uint kParamEquatorTemperatureC = 23;
static const uint kParamPoleTemperatureC = 24;
static const uint kParamTemperatureVariationC = 25;
static const uint kParamLapseRateCPerKilometer = 26;
static const uint kParamBoundaryWidthDot = 27;
static const uint kParamConvergenceReferenceSpeed = 28;
static const uint kParamOceanicConvergenceScale = 29;
static const uint kParamConvergenceUpliftMeters = 30;
static const uint kParamPlateCount = 31;
static const uint kParamHotspotCount = 32;
static const uint kParamHotspotAgeSteps = 33;
static const uint kParamRainShadowStrength = 34;
static const uint kParamRainShadowStepMeters = 35;
static const uint kParamRainShadowStepGrowth = 36;
static const uint kParamRainShadowThresholdMeters = 37;
static const uint kParamRainShadowRangeMeters = 38;
static const uint kParamRainShadowSteps = 39;
static const uint kParamWindBandTransitionDegrees = 40;
// Two distinct 64-bit seeds, matching the CPU source's own two-level
// seed resolution (see GpuFieldGenerator.cpp): kParamTopSeed is
// AnalyticTerrainDesc::seed verbatim (macro/warp/detail/local-mountain
// bands); kParamGlobalSeed is GlobalTerrainFields' already-resolved
// GlobalTerrainFieldDesc::seed (continental/climate/global-mountain
// bands) -- NOT the same value when AnalyticTerrainDesc::global.seed
// was left at its "derive from the top-level seed" default of 0.
static const uint kParamTopSeedLo = 41;
static const uint kParamTopSeedHi = 42;
static const uint kParamGlobalSeedLo = 43;
static const uint kParamGlobalSeedHi = 44;

static const uint kMaxHotspotAgeSteps = 6u;
static const uint kPlateStrideBytes = 36u;
static const uint kHotspotStrideBytes = 136u;
static const uint kSampleStrideBytes = 32u;

[[vk::binding(0, 0)]]
ByteAddressBuffer g_params : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_plates : register(t1);
[[vk::binding(2, 0)]]
ByteAddressBuffer g_hotspots : register(t2);
[[vk::binding(3, 0)]]
RWByteAddressBuffer g_output : register(u3);

// float4, not float3, for every basis vector below: HLSL/DXC pads a
// bare float3 push-constant member out to a 16-byte slot anyway (the
// same reason TerrainPreviewRenderer.cpp's DrawConstants packs each of
// its basis vectors as an explicit float4) -- declaring float4 up front
// makes the actual 160-byte layout (kPushConstantDwords in
// GpuFieldGenerator.cpp) match what DXC emits exactly, instead of
// silently padding past whatever smaller size C++ assumed.
struct PushConstants
{
    uint resolution;
    float spacingMeters;
    float footprintMeters;
    uint morphToCoarser;
    float morphStartHalfExtentMeters;
    float morphEndHalfExtentMeters;
    float coarseSpacingMeters;
    float coarseFootprintMeters;
    uint originX;
    uint originY;
    float fineNormalFootprintMeters;
    float fineNormalEpsilonMeters;
    uint regionX;
    uint regionY;
    uint regionWidth;
    uint regionHeight;
    // fineUp.w / fineEast.w carry the clipmap window center's X/Y
    // offset inside this stable lattice frame.
    float4 fineUp;
    float4 fineEast;
    float4 fineNorth;
    float4 coarseUp;
    float4 coarseEast;
    float4 coarseNorth;
};
[[vk::push_constant]] PushConstants g_pc;

float ParamFloat(uint index) { return asfloat(g_params.Load(index * 4u)); }
uint ParamUint(uint index) { return g_params.Load(index * 4u); }

float3 PlateSeedDirection(uint i) { return asfloat(g_plates.Load3(i * kPlateStrideBytes + 0u)); }
float PlateContinentalBias(uint i) { return asfloat(g_plates.Load(i * kPlateStrideBytes + 12u)); }
float3 PlateEulerVector(uint i) { return asfloat(g_plates.Load3(i * kPlateStrideBytes + 16u)); }
float PlateSizeBias(uint i) { return asfloat(g_plates.Load(i * kPlateStrideBytes + 28u)); }
float PlateIsContinental(uint i) { return asfloat(g_plates.Load(i * kPlateStrideBytes + 32u)); }

float3 HotspotMantlePosition(uint h) { return asfloat(g_hotspots.Load3(h * kHotspotStrideBytes + 0u)); }
float HotspotBoundingCosine(uint h) { return asfloat(g_hotspots.Load(h * kHotspotStrideBytes + 12u)); }
float3 HotspotChainPoint(uint h, uint k) { return asfloat(g_hotspots.Load3(h * kHotspotStrideBytes + 16u + k * 20u)); }
float HotspotChainAmplitude(uint h, uint k) { return asfloat(g_hotspots.Load(h * kHotspotStrideBytes + 16u + k * 20u + 12u)); }
float HotspotChainChordRadius(uint h, uint k) { return asfloat(g_hotspots.Load(h * kHotspotStrideBytes + 16u + k * 20u + 16u)); }

// === Low-level noise -- bit-exact port of ProceduralNoise.hpp's 64-bit
// hash (Mix64/HashLattice/MakeNoiseCell/ValueNoise3D/VectorNoise3D),
// using real uint64_t (Vulkan shaderInt64, enabled in VulkanDevice.cpp).
// This is deliberately NOT the "any good hash will do" simplification
// used for the GPU-only hydrology relaxation passes: the CPU map and
// hydrology sample this exact same terrain, so the base elevation/
// climate noise field has to match, or GPU-rendered terrain would show
// different coastlines/mountains than the CPU-rendered map claims are
// there. uint64_t literals are built from hi/lo 32-bit halves (MakeU64)
// rather than relying on a specific HLSL 64-bit literal suffix. ===

uint64_t MakeU64(uint hi, uint lo)
{
    return (uint64_t(hi) << 32) | uint64_t(lo);
}

uint64_t Mix64(uint64_t value)
{
    value += MakeU64(0x9E3779B9u, 0x7F4A7C15u);
    value = (value ^ (value >> 30)) * MakeU64(0xBF58476Du, 0x1CE4E5B9u);
    value = (value ^ (value >> 27)) * MakeU64(0x94D049BBu, 0x133111EBu);
    return value ^ (value >> 31);
}

uint64_t ParamU64(uint loIndex, uint hiIndex)
{
    return MakeU64(ParamUint(hiIndex), ParamUint(loIndex));
}

// Quintic fade, matching ProceduralNoise.hpp's detail::Smooth -- used for
// noise interpolation. NOT the same curve as SmoothStepCubic below, which
// matches TerrainFields.cpp's own cubic smoothstep used for biome
// classification -- the two must stay distinct, mirroring the CPU source.
float Smooth(float x)
{
    x = saturate(x);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

float SmoothStepCubic(float edge0, float edge1, float value)
{
    if (edge1 <= edge0)
    {
        return value >= edge1 ? 1.0 : 0.0;
    }
    float t = saturate((value - edge0) / (edge1 - edge0));
    return t * t * (3.0 - 2.0 * t);
}

// Top 24 bits of the 64-bit hash -> [-1,1). The CPU keeps the top 53 bits
// (an exact double mantissa); a well-mixed hash's bits are equally
// well-distributed regardless of which contiguous span is kept, so
// narrowing to a 24-bit float mantissa here is extra float-precision
// jitter on the same value, not a different one.
float HashToUnit(uint64_t hash)
{
    uint top = uint(hash >> 40);
    return float(top) * (1.0 / 16777216.0) * 2.0 - 1.0;
}

// Sign-extends a bounded-range int lattice coordinate to uint64_t,
// matching ProceduralNoise.hpp's `static_cast<u64>(x)` where x is i64 --
// this codebase's noise-domain coordinates never approach the i32 range
// limit (see the file header's frequency-magnitude table), so int32
// ->int64_t->uint64_t reproduces the CPU's i64->u64 cast exactly for
// every value actually reached here.
uint64_t AxisCoord(int x)
{
    return uint64_t(int64_t(x));
}

void NoiseAxisHashes(
    int3 p0, out uint64_t x0, out uint64_t x1, out uint64_t y0, out uint64_t y1, out uint64_t z0, out uint64_t z1)
{
    uint64_t xSalt = MakeU64(0x632BE59Bu, 0xD9B4E019u);
    uint64_t ySalt = MakeU64(0x8CB92BA7u, 0x2F3D8DD7u);
    uint64_t zSalt = MakeU64(0x58F38DEDu, 0x8C5A935Fu);

    x0 = Mix64(AxisCoord(p0.x) + xSalt);
    x1 = Mix64(AxisCoord(p0.x + 1) + xSalt);
    y0 = Mix64(AxisCoord(p0.y) + ySalt);
    y1 = Mix64(AxisCoord(p0.y + 1) + ySalt);
    z0 = Mix64(AxisCoord(p0.z) + zSalt);
    z1 = Mix64(AxisCoord(p0.z + 1) + zSalt);
}

float InterpolateCellScalar(float v0, float v1, float v2, float v3, float v4, float v5, float v6, float v7, float3 blend)
{
    float x00 = lerp(v0, v1, blend.x);
    float x10 = lerp(v2, v3, blend.x);
    float x01 = lerp(v4, v5, blend.x);
    float x11 = lerp(v6, v7, blend.x);
    float y0 = lerp(x00, x10, blend.y);
    float y1 = lerp(x01, x11, blend.y);
    return lerp(y0, y1, blend.z);
}

float3 InterpolateCellVector(float3 v0, float3 v1, float3 v2, float3 v3, float3 v4, float3 v5, float3 v6, float3 v7, float3 blend)
{
    float3 x00 = lerp(v0, v1, blend.x);
    float3 x10 = lerp(v2, v3, blend.x);
    float3 x01 = lerp(v4, v5, blend.x);
    float3 x11 = lerp(v6, v7, blend.x);
    float3 y0 = lerp(x00, x10, blend.y);
    float3 y1 = lerp(x01, x11, blend.y);
    return lerp(y0, y1, blend.z);
}

float ValueNoise3D(float3 position, uint64_t seed)
{
    int3 p0 = int3(floor(position));
    float3 f = position - float3(p0);
    float3 blend = float3(Smooth(f.x), Smooth(f.y), Smooth(f.z));

    uint64_t x0, x1, y0, y1, z0, z1;
    NoiseAxisHashes(p0, x0, x1, y0, y1, z0, z1);

    float h0 = HashToUnit(Mix64(seed ^ x0 ^ y0 ^ z0));
    float h1 = HashToUnit(Mix64(seed ^ x1 ^ y0 ^ z0));
    float h2 = HashToUnit(Mix64(seed ^ x0 ^ y1 ^ z0));
    float h3 = HashToUnit(Mix64(seed ^ x1 ^ y1 ^ z0));
    float h4 = HashToUnit(Mix64(seed ^ x0 ^ y0 ^ z1));
    float h5 = HashToUnit(Mix64(seed ^ x1 ^ y0 ^ z1));
    float h6 = HashToUnit(Mix64(seed ^ x0 ^ y1 ^ z1));
    float h7 = HashToUnit(Mix64(seed ^ x1 ^ y1 ^ z1));

    return InterpolateCellScalar(h0, h1, h2, h3, h4, h5, h6, h7, blend);
}

// Ports VectorNoise3D's packing of three disjoint 21-bit channels out of
// one 64-bit corner hash (not three independent Mix64 calls).
float3 CornerVector(uint64_t hash)
{
    uint64_t mask = MakeU64(0x1Fu, 0xFFFFFu); // (1<<21)-1
    float scale = 2.0 / 2097151.0; // 2 / mask, mask as a float

    float cx = float(uint(hash & mask)) * scale - 1.0;
    float cy = float(uint((hash >> 21) & mask)) * scale - 1.0;
    float cz = float(uint((hash >> 42) & mask)) * scale - 1.0;
    return float3(cx, cy, cz);
}

float3 VectorNoise3D(float3 position, uint64_t seed)
{
    int3 p0 = int3(floor(position));
    float3 f = position - float3(p0);
    float3 blend = float3(Smooth(f.x), Smooth(f.y), Smooth(f.z));

    uint64_t x0, x1, y0, y1, z0, z1;
    NoiseAxisHashes(p0, x0, x1, y0, y1, z0, z1);

    float3 v0 = CornerVector(seed ^ x0 ^ y0 ^ z0);
    float3 v1 = CornerVector(seed ^ x1 ^ y0 ^ z0);
    float3 v2 = CornerVector(seed ^ x0 ^ y1 ^ z0);
    float3 v3 = CornerVector(seed ^ x1 ^ y1 ^ z0);
    float3 v4 = CornerVector(seed ^ x0 ^ y0 ^ z1);
    float3 v5 = CornerVector(seed ^ x1 ^ y0 ^ z1);
    float3 v6 = CornerVector(seed ^ x0 ^ y1 ^ z1);
    float3 v7 = CornerVector(seed ^ x1 ^ y1 ^ z1);

    return InterpolateCellVector(v0, v1, v2, v3, v4, v5, v6, v7, blend);
}

float DetailWeight(float wavelengthMeters, float footprintMeters)
{
    if (footprintMeters <= 0.0) return 1.0;
    float lower = footprintMeters * 2.0;
    float upper = footprintMeters * 4.0;
    if (wavelengthMeters <= lower) return 0.0;
    if (wavelengthMeters >= upper) return 1.0;
    return Smooth((wavelengthMeters - lower) / (upper - lower));
}

float SampleBand(float3 direction, float planetRadiusMeters, float wavelengthMeters, uint64_t seed)
{
    if (wavelengthMeters <= 0.0 || planetRadiusMeters <= 0.0) return 0.0;
    float frequency = planetRadiusMeters / wavelengthMeters;
    return ValueNoise3D(direction * frequency, seed);
}

float RidgedBand(float3 direction, float planetRadiusMeters, float wavelengthMeters, uint64_t seed)
{
    float noise = SampleBand(direction, planetRadiusMeters, wavelengthMeters, seed);
    float ridge = 1.0 - abs(noise);
    return ridge * ridge * ridge;
}

// === Octave band recurrence (matches AnalyticTerrainSource.cpp's
// `prepare` lambda: wavelength/lacunarity, amplitude*0.5 each octave, and
// its seed formula `(baseSeed ^ salt) + (index+1)*goldenRatio64`) ===

void OctaveBand(
    uint index, float baseWavelength, float baseAmplitude, float lacunarity,
    uint64_t baseSeed, uint64_t salt,
    out float wavelength, out float amplitude, out uint64_t seed)
{
    wavelength = baseWavelength;
    amplitude = baseAmplitude;
    for (uint i = 0; i < index; ++i)
    {
        wavelength /= lacunarity;
        amplitude *= 0.5;
    }
    seed = (baseSeed ^ salt) + uint64_t(index + 1u) * MakeU64(0x9E3779B9u, 0x7F4A7C15u);
}

// === Tectonics (elevation-relevant subset only: convergenceMask and
// plateBiasMeters -- divergence/transform/continental-flag outputs exist
// only for the CPU-side 2D map's boundary classification, unused here) ===
)" R"(
float3 SampleTectonicConvergenceAndBias(float3 direction, out float plateBiasMeters)
{
    uint plateCount = ParamUint(kParamPlateCount);
    uint nearest = 0u;
    uint second = 0u;
    float d0 = -2.0;
    float d1 = -2.0;

    for (uint i = 0; i < plateCount; ++i)
    {
        float d = dot(direction, PlateSeedDirection(i)) + PlateSizeBias(i);
        if (d > d0)
        {
            second = nearest; d1 = d0; nearest = i; d0 = d;
        }
        else if (d > d1)
        {
            second = i; d1 = d;
        }
    }

    if (plateCount <= 1u)
    {
        plateBiasMeters = PlateContinentalBias(nearest);
        return 0.0;
    }

    float boundaryWidthDot = ParamFloat(kParamBoundaryWidthDot);
    float boundaryMask = Smooth(1.0 - (d0 - d1) / max(boundaryWidthDot, 1.0e-9));

    float convergenceMask = 0.0;
    if (boundaryMask > 0.0)
    {
        float3 towardSecond = PlateSeedDirection(second) - PlateSeedDirection(nearest);
        float3 tangentToward = towardSecond - direction * dot(towardSecond, direction);
        float tangentLenSq = dot(tangentToward, tangentToward);
        if (tangentLenSq > 0.0)
        {
            float3 normal = tangentToward / sqrt(tangentLenSq);
            float3 vNearest = cross(PlateEulerVector(nearest), direction);
            float3 vSecond = cross(PlateEulerVector(second), direction);
            float3 relative = vNearest - vSecond;

            float normalSpeed = dot(relative, normal);
            float convergence = max(0.0, -normalSpeed);

            bool eitherContinental = (PlateIsContinental(nearest) > 0.5) || (PlateIsContinental(second) > 0.5);
            float collisionScale = eitherContinental ? 1.0 : ParamFloat(kParamOceanicConvergenceScale);

            float referenceSpeed = max(ParamFloat(kParamConvergenceReferenceSpeed), 1.0e-9);
            convergenceMask = boundaryMask * Smooth(convergence / referenceSpeed) * collisionScale;
        }
    }

    float crossBlend = Smooth(0.5 + 0.5 * (d1 - d0) / max(boundaryWidthDot, 1.0e-9));
    plateBiasMeters = lerp(PlateContinentalBias(nearest), PlateContinentalBias(second), crossBlend);
    return convergenceMask;
}

float HotspotElevationMeters(float3 direction)
{
    float sum = 0.0;
    uint hotspotCount = ParamUint(kParamHotspotCount);
    uint ageSteps = min(ParamUint(kParamHotspotAgeSteps), kMaxHotspotAgeSteps);

    for (uint h = 0; h < hotspotCount; ++h)
    {
        float3 mantlePos = HotspotMantlePosition(h);
        float boundingCosine = HotspotBoundingCosine(h);
        if (dot(direction, mantlePos) < boundingCosine) continue;

        for (uint k = 0; k < ageSteps; ++k)
        {
            float radius = HotspotChainChordRadius(h, k);
            if (radius <= 0.0) continue;
            float3 delta = direction - HotspotChainPoint(h, k);
            float chordSquared = dot(delta, delta);
            float t = 1.0 - chordSquared / (radius * radius);
            if (t <= 0.0) continue;
            float falloff = Smooth(t);
            sum += HotspotChainAmplitude(h, k) * falloff * falloff;
        }
    }

    return sum;
}

float ContinentalSignal(float3 direction)
{
    float wavelength = ParamFloat(kParamContinentalWavelengthMeters);
    float radius = ParamFloat(kParamPlanetRadiusMeters);
    uint64_t globalSeed = ParamU64(kParamGlobalSeedLo, kParamGlobalSeedHi);
    float primary = SampleBand(direction, radius, wavelength, globalSeed);
    float secondary = SampleBand(direction, radius, wavelength * 0.53,
        globalSeed ^ MakeU64(0x58F38DEDu, 0x8C5A935Fu));
    return primary * 0.76 + secondary * 0.24;
}

// Cheap, footprint-independent coarse elevation estimate -- GPU port of
// GlobalTerrainFields::PlateElevationEstimateMeters, used only by the
// rain-shadow upwind probe below (never recursively calls the full
// sample function).
float PlateElevationEstimateMeters(float3 direction)
{
    float plateBiasMeters;
    float convergenceMask = SampleTectonicConvergenceAndBias(direction, plateBiasMeters);

    float continentalAmplitudeSafe = max(ParamFloat(kParamContinentalAmplitudeMeters), 1.0e-9);
    float blendedSignal = ContinentalSignal(direction) +
        (plateBiasMeters / continentalAmplitudeSafe) * ParamFloat(kParamTectonicContinentInfluence);

    float continentalElevation = blendedSignal * ParamFloat(kParamContinentalAmplitudeMeters) +
        ParamFloat(kParamContinentalBiasMeters);

    float convergenceBump = convergenceMask * ParamFloat(kParamConvergenceUpliftMeters);
    float hotspotBump = HotspotElevationMeters(direction);

    return continentalElevation + convergenceBump + hotspotBump;
}

struct GlobalSample
{
    float coarseElevationMeters;
    float landMask;
    float temperatureC;
    float humidity;
    float precipitation;
    float convergenceMask;
    float hotspotElevationMeters;
};

GlobalSample SampleGlobalFields(float3 direction, float footprintMeters)
{
    float radius = ParamFloat(kParamPlanetRadiusMeters);
    uint64_t globalSeed = ParamU64(kParamGlobalSeedLo, kParamGlobalSeedHi);

    float continentalWeight = DetailWeight(ParamFloat(kParamContinentalWavelengthMeters), footprintMeters);
    float mountainWeight = DetailWeight(ParamFloat(kParamGlobalMountainWavelengthMeters), footprintMeters);

    float plateBiasMeters;
    float convergenceMask = SampleTectonicConvergenceAndBias(direction, plateBiasMeters);

    float continentSignal = ContinentalSignal(direction);
    float continentalAmplitudeSafe = max(ParamFloat(kParamContinentalAmplitudeMeters), 1.0e-9);
    float blendedSignal = continentSignal +
        (plateBiasMeters / continentalAmplitudeSafe) * ParamFloat(kParamTectonicContinentInfluence);

    float continentalElevation = (blendedSignal * ParamFloat(kParamContinentalAmplitudeMeters) +
        ParamFloat(kParamContinentalBiasMeters)) * continentalWeight;

    float landMask = Smooth((blendedSignal + 0.15) / 0.55);

    float globalMountainAmplitude = ParamFloat(kParamGlobalMountainAmplitudeMeters);
    float globalMountainWavelength = ParamFloat(kParamGlobalMountainWavelengthMeters);

    float mountainRidges = 0.0;
    if (landMask > 0.0 && mountainWeight > 0.0 && globalMountainAmplitude > 0.0)
    {
        mountainRidges = RidgedBand(direction, radius, globalMountainWavelength,
            globalSeed ^ MakeU64(0xD1B54A32u, 0xD192ED03u));
    }

    float mountainModulation = 0.0;
    if (mountainRidges > 0.0)
    {
        mountainModulation = saturate(
            SampleBand(direction, radius, globalMountainWavelength * 2.4,
                globalSeed ^ MakeU64(0x94D049BBu, 0x133111EBu)) * 0.5 + 0.5);
    }

    float mountainElevation = mountainRidges * mountainModulation * landMask * convergenceMask *
        globalMountainAmplitude * mountainWeight;

    float coarseElevation = continentalElevation + mountainElevation;

    float latitude = saturate(abs(direction.y));
    float climateNoise = SampleBand(direction, radius, ParamFloat(kParamClimateWavelengthMeters),
        globalSeed ^ MakeU64(0xA24BAED4u, 0x963EE407u));
    float moistureNoise = SampleBand(direction, radius, ParamFloat(kParamClimateWavelengthMeters) * 0.72,
        globalSeed ^ MakeU64(0x9FB21C65u, 0x1E98DF25u));
    float continentalityNoise = SampleBand(direction, radius, ParamFloat(kParamClimateWavelengthMeters) * 1.35,
        globalSeed ^ MakeU64(0xC13FA9A9u, 0x02A6328Fu));

    float polarFactor = pow(latitude, 1.18);
    float equatorTemp = ParamFloat(kParamEquatorTemperatureC);
    float temperature = equatorTemp + (ParamFloat(kParamPoleTemperatureC) - equatorTemp) * polarFactor;
    temperature += climateNoise * ParamFloat(kParamTemperatureVariationC);
    temperature -= max(coarseElevation - ParamFloat(kParamSeaLevelMeters), 0.0) / 1000.0 * ParamFloat(kParamLapseRateCPerKilometer);

    float continentality = saturate(0.5 + continentalityNoise * 0.28 +
        max(coarseElevation - ParamFloat(kParamSeaLevelMeters), 0.0) / 8000.0);
    if (coarseElevation < ParamFloat(kParamSeaLevelMeters))
    {
        continentality *= 0.2;
    }

    float equatorialMoisture = 1.0 - abs(latitude - 0.16);
    float humidity = saturate(0.57 + moistureNoise * 0.30 - continentality * 0.18 + equatorialMoisture * 0.08);
    if (coarseElevation < ParamFloat(kParamSeaLevelMeters))
    {
        humidity = max(humidity, 0.88);
    }

    float precipitation = saturate(humidity * (0.86 - continentality * 0.25) * (0.88 + climateNoise * 0.12));

    GlobalSample result;
    result.coarseElevationMeters = coarseElevation;
    result.landMask = landMask;
    result.temperatureC = temperature;
    result.humidity = humidity;
    result.precipitation = precipitation;
    result.convergenceMask = convergenceMask;
    result.hotspotElevationMeters = HotspotElevationMeters(direction);
    return result;
}

// === Local mountain relief (AnalyticTerrainSource::MountainShape) ===
)" R"(
float ShapeRidge(float ridge)
{
    return ridge + 0.85 * ridge * (1.0 - ridge);
}

float MountainShape(float3 direction, float footprintMeters)
{
    uint octaves = ParamUint(kParamMountainOctaves);
    float reliefMeters = ParamFloat(kParamMountainReliefMeters);
    if (octaves == 0u || reliefMeters == 0.0) return 0.0;

    float radius = ParamFloat(kParamPlanetRadiusMeters);
    uint64_t topSeed = ParamU64(kParamTopSeedLo, kParamTopSeedHi);
    float baseWavelength = ParamFloat(kParamMountainWavelengthMeters);
    float warpFootprintScale = ParamFloat(kParamWarpFootprintScale);
    float footprint = footprintMeters * warpFootprintScale * 2.0;

    if (DetailWeight(baseWavelength, footprint) <= 0.0)
    {
        return ShapeRidge(0.5);
    }

    float3 warped = direction;
    float warpAmplitude = ParamFloat(kParamWarpAmplitudeMeters);
    if (warpAmplitude > 0.0)
    {
        float warpWeight = DetailWeight(ParamFloat(kParamWarpWavelengthMeters), footprintMeters);
        float warpFrequency = ParamFloat(kParamWarpFrequency);
        warped += VectorNoise3D(direction * warpFrequency, topSeed ^ MakeU64(0x8CB92BA7u, 0x2F3D8DD7u)) *
            (warpAmplitude * warpWeight / radius);
    }

    float sum = 0.0;
    float feedback = 1.0;

    [loop]
    for (uint i = 0; i < octaves; ++i)
    {
        float bandWavelength, bandAmplitude;
        uint64_t bandSeed;
        OctaveBand(i, baseWavelength, 1.0, 2.03, topSeed, MakeU64(0xDB4F0B91u, 0x75AE2165u),
            bandWavelength, bandAmplitude, bandSeed);

        float weight = DetailWeight(bandWavelength, footprint);
        if (weight <= 0.0)
        {
            uint remaining = octaves - i;
            float tail = bandAmplitude * 2.0 * (1.0 - exp2(-float(remaining)));
            sum += 0.5 * feedback * tail;
            break;
        }

        float noise = ValueNoise3D(warped * (radius / bandWavelength), bandSeed);
        float ridge = 1.0 - abs(noise);
        float signal = ridge * ridge;
        sum += lerp(0.5, signal, weight) * feedback * bandAmplitude;
        feedback *= lerp(1.0, saturate(signal * 2.0), weight);

        warped = float3(
            0.36 * warped.x + 0.48 * warped.y - 0.80 * warped.z,
            -0.80 * warped.x + 0.60 * warped.y,
            0.48 * warped.x + 0.64 * warped.y + 0.60 * warped.z);
    }

    return ShapeRidge(sum / max(ParamFloat(kParamMountainNormalization), 1.0));
}

// === Rain shadow probe helpers (AnalyticTerrainSource.cpp) ===

float ZonalWindSign(float latitudeDeg, float transitionDegrees)
{
    float absLatitude = abs(latitudeDeg);
    float halfWidth = max(transitionDegrees, 0.1) * 0.5;
    float tradeToWesterly = Smooth((absLatitude - (30.0 - halfWidth)) / (2.0 * halfWidth));
    float westerlyToPolar = Smooth((absLatitude - (60.0 - halfWidth)) / (2.0 * halfWidth));
    float throughWesterlies = lerp(-1.0, 1.0, tradeToWesterly);
    return lerp(throughWesterlies, -1.0, westerlyToPolar);
}

void MakeSurfaceFrame(float3 up, out float3 outEast, out float3 outNorth)
{
    float3 reference = abs(up.y) < 0.95 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    outEast = normalize(cross(reference, up));
    outNorth = normalize(cross(up, outEast));
}

// Ports orbit::world::DirectionAtSurfaceOffset -- see the comment on
// SurfaceDirectionForOffsetFromBasis in TerrainPreviewRenderer.cpp, whose
// identical closed-form rotation this matches (Rodrigues' formula for a
// rotation of `up` around an axis perpendicular to it simplifies to
// exactly this).
float3 DirectionAtSurfaceOffset(float3 up, float3 east, float3 north, float2 offsetMeters, float planetRadiusMeters)
{
    float distanceMeters = length(offsetMeters);
    if (distanceMeters <= 0.0001) return up;
    float3 tangentDirection = normalize(east * offsetMeters.x + north * offsetMeters.y);
    float angle = distanceMeters / planetRadiusMeters;
    return normalize(up * cos(angle) + tangentDirection * sin(angle));
}

// === Biome classification (TerrainFields.cpp::ClassifyBiomeWeights) ===

void ClassifyBiomeWeights(
    float temperatureC, float humidity, float precipitation,
    float elevationMeters, float seaLevelMeters,
    out float4 biome0, out float4 biome1)
{
    float relativeElevation = elevationMeters - seaLevelMeters;

    float land = SmoothStepCubic(-80.0, 120.0, relativeElevation);
    float ocean = 1.0 - land;
    float alpine = land * SmoothStepCubic(1800.0, 3800.0, relativeElevation);
    float nonAlpine = land * (1.0 - alpine);

    float precip = saturate(precipitation);
    float hum = saturate(humidity);

    float hot = SmoothStepCubic(18.0, 31.0, temperatureC);
    float cold = 1.0 - SmoothStepCubic(-8.0, 8.0, temperatureC);
    float cool = (1.0 - cold) * (1.0 - SmoothStepCubic(10.0, 19.0, temperatureC));
    float temperate = SmoothStepCubic(4.0, 14.0, temperatureC) * (1.0 - SmoothStepCubic(23.0, 31.0, temperatureC));

    float dry = 1.0 - SmoothStepCubic(0.22, 0.48, precip);
    float moist = SmoothStepCubic(0.35, 0.68, precip);
    float saturatedWet = SmoothStepCubic(0.72, 0.94, hum) * SmoothStepCubic(0.62, 0.88, precip);
    float lowland = 1.0 - SmoothStepCubic(350.0, 1100.0, abs(relativeElevation));

    float w0 = max(ocean * 3.0, 0.0);
    float w1 = max(nonAlpine * hot * dry * 1.7, 0.0);
    float w2 = max(nonAlpine * (0.35 + temperate) * (1.0 - 0.65 * moist) * (1.0 - 0.7 * dry), 0.0);
    float w3 = max(nonAlpine * temperate * moist * 1.5, 0.0);
    float w4 = max(nonAlpine * cool * moist * 1.35, 0.0);
    float w5 = max(nonAlpine * cold * (0.5 + 0.5 * (1.0 - dry)) * 1.5, 0.0);
    float w6 = max(alpine * 2.2, 0.0);
    float w7 = max(nonAlpine * saturatedWet * lowland * (1.0 - cold) * 1.4, 0.0);

    float sum = w0 + w1 + w2 + w3 + w4 + w5 + w6 + w7;
    if (sum <= 1.0e-6)
    {
        biome0 = float4(0.0, 0.0, 1.0, 0.0);
        biome1 = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    float inv = 1.0 / sum;
    biome0 = float4(w0, w1, w2, w3) * inv;
    biome1 = float4(w4, w5, w6, w7) * inv;
}

// === Full sample (AnalyticTerrainSource::Sample) ===

float LimitElevation(float elevationMeters, float seaLevelMeters, float maxElevation)
{
    float shoulder = maxElevation * 0.75;
    float relative = elevationMeters - seaLevelMeters;
    if (relative <= shoulder) return elevationMeters;
    float headroom = maxElevation - shoulder;
    float x = (relative - shoulder) / headroom;
    return seaLevelMeters + shoulder + headroom * (1.0 - 1.0 / (1.0 + x + x * x));
}
)" R"(
struct FullSample
{
    float elevationMeters;
    float4 biome0;
    float4 biome1;
    float standingWaterDepthMeters;
};

FullSample GenerateSample(float3 direction, float footprintMeters)
{
    float radius = ParamFloat(kParamPlanetRadiusMeters);
    float seaLevel = ParamFloat(kParamSeaLevelMeters);
    float maxElevation = ParamFloat(kParamMaximumElevationAboveSeaLevelMeters);
    uint64_t topSeed = ParamU64(kParamTopSeedLo, kParamTopSeedHi);

    GlobalSample global = SampleGlobalFields(direction, footprintMeters);

    float macroWeight = DetailWeight(ParamFloat(kParamMacroWavelengthMeters), footprintMeters);
    float macroNoise = macroWeight > 0.0
        ? SampleBand(direction, radius, ParamFloat(kParamMacroWavelengthMeters),
            topSeed ^ MakeU64(0x632BE59Bu, 0xD9B4E019u))
        : 0.0;
    float elevation = LimitElevation(
        global.coarseElevationMeters + macroNoise * ParamFloat(kParamMacroAmplitudeMeters) * macroWeight,
        seaLevel, maxElevation);

    float coastMask = Smooth((elevation - seaLevel) / 700.0);
    float rangeMask = global.convergenceMask;
    float mountainReliefMeters = ParamFloat(kParamMountainReliefMeters);
    uint mountainOctaves = ParamUint(kParamMountainOctaves);
    float mountainMask = (mountainReliefMeters > 0.0 && mountainOctaves > 0u)
        ? global.landMask * coastMask * rangeMask : 0.0;
    float ceiling = seaLevel + maxElevation;

    if (mountainMask > 0.0 && mountainReliefMeters > 0.0)
    {
        float availableRelief = min(mountainReliefMeters, ceiling - elevation);
        elevation += mountainMask * availableRelief * MountainShape(direction, footprintMeters);
    }

    if (global.hotspotElevationMeters > 0.0)
    {
        float headroom = max(0.0, ceiling - elevation);
        elevation += min(global.hotspotElevationMeters, headroom);
    }

    float coarseElevation = elevation;
    float detailAmplitude = ParamFloat(kParamDetailAmplitudeMeters);
    float detailGain = min(1.0, (ceiling - elevation) / max(detailAmplitude * 2.0, 1.0));
    float hillDetailWeight = 1.0 - Smooth(mountainMask / 0.65);

    uint detailOctaves = ParamUint(kParamDetailOctaves);
    float detailBaseWavelength = ParamFloat(kParamDetailWavelengthMeters);
    float mountainWavelength = ParamFloat(kParamMountainWavelengthMeters);

    if (detailAmplitude > 0.0)
    {
        [loop]
        for (uint i = 0; i < detailOctaves; ++i)
        {
            float bandWavelength, bandAmplitude;
            uint64_t bandSeed;
            OctaveBand(i, detailBaseWavelength, detailAmplitude, 2.0, topSeed, MakeU64(0u, 0u),
                bandWavelength, bandAmplitude, bandSeed);

            float weight = DetailWeight(bandWavelength, footprintMeters);
            if (weight <= 0.0) break;

            float landformWeight = bandWavelength > mountainWavelength / 32.0 ? hillDetailWeight : 1.0;
            if (landformWeight <= 0.0) continue;

            elevation += ValueNoise3D(direction * (radius / bandWavelength), bandSeed) *
                bandAmplitude * weight * detailGain * landformWeight;
        }
    }

    float temperatureC = global.temperatureC -
        (max(elevation - seaLevel, 0.0) - max(global.coarseElevationMeters - seaLevel, 0.0)) *
        ParamFloat(kParamLapseRateCPerKilometer) / 1000.0;
    float humidity = global.humidity;
    float precipitation = global.precipitation;

    float rainShadowStrength = ParamFloat(kParamRainShadowStrength);
    uint rainShadowSteps = ParamUint(kParamRainShadowSteps);
    if (rainShadowStrength > 0.0 && rainShadowSteps > 0u)
    {
        float latitudeDeg = asin(clamp(direction.y, -1.0, 1.0)) * (180.0 / 3.14159265358979);
        float windSign = ZonalWindSign(latitudeDeg, ParamFloat(kParamWindBandTransitionDegrees));

        float3 frameEast, frameNorth;
        MakeSurfaceFrame(direction, frameEast, frameNorth);

        float blockingHeight = -1.0e30;
        float stepDistance = ParamFloat(kParamRainShadowStepMeters);
        float stepGrowth = ParamFloat(kParamRainShadowStepGrowth);

        [loop]
        for (uint step = 0; step < rainShadowSteps; ++step)
        {
            float3 upwindDirection = DirectionAtSurfaceOffset(
                direction, frameEast, frameNorth, float2(-windSign * stepDistance, 0.0), radius);
            blockingHeight = max(blockingHeight, PlateElevationEstimateMeters(upwindDirection));
            stepDistance *= stepGrowth;
        }

        float shadow = Smooth((blockingHeight - coarseElevation - ParamFloat(kParamRainShadowThresholdMeters)) /
            max(ParamFloat(kParamRainShadowRangeMeters), 1.0));

        precipitation *= (1.0 - rainShadowStrength * shadow);
        humidity *= (1.0 - rainShadowStrength * 0.7 * shadow);
    }

    FullSample result;
    result.elevationMeters = elevation;
    ClassifyBiomeWeights(temperatureC, humidity, precipitation, elevation, seaLevel, result.biome0, result.biome1);
    result.standingWaterDepthMeters = max(seaLevel - elevation, 0.0);
    return result;
}

FullSample LerpFullSample(FullSample a, FullSample b, float t)
{
    FullSample result;
    result.elevationMeters = lerp(a.elevationMeters, b.elevationMeters, t);
    result.biome0 = lerp(a.biome0, b.biome0, t);
    result.biome1 = lerp(a.biome1, b.biome1, t);
    result.standingWaterDepthMeters = lerp(a.standingWaterDepthMeters, b.standingWaterDepthMeters, t);
    return result;
}

// === Fine-detail shading slope (mirrors
// terrain_stream::TerrainSampleStreamer::SampleFineSlope) ===

float2 SampleFineSlope(float3 up, float3 east, float3 north, float2 offsetMeters, float footprintMeters, float epsilonMeters, float planetRadiusMeters)
{
    float3 centerDir = DirectionAtSurfaceOffset(up, east, north, offsetMeters, planetRadiusMeters);
    float3 eastDir = DirectionAtSurfaceOffset(up, east, north, offsetMeters + float2(epsilonMeters, 0.0), planetRadiusMeters);
    float3 northDir = DirectionAtSurfaceOffset(up, east, north, offsetMeters + float2(0.0, epsilonMeters), planetRadiusMeters);

    float centerElevation = GenerateSample(centerDir, footprintMeters).elevationMeters;
    float eastElevation = GenerateSample(eastDir, footprintMeters).elevationMeters;
    float northElevation = GenerateSample(northDir, footprintMeters).elevationMeters;

    return float2(
        (eastElevation - centerElevation) / epsilonMeters,
        (northElevation - centerElevation) / epsilonMeters);
}

uint PackUnorm4x8(float4 v)
{
    uint4 q = uint4(round(saturate(v) * 255.0));
    return q.x | (q.y << 8u) | (q.z << 16u) | (q.w << 24u);
}

uint WrapIndex(int value, uint size)
{
    int m = value % int(size);
    if (m < 0) m += int(size);
    return uint(m);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g_pc.regionWidth || dispatchId.y >= g_pc.regionHeight)
    {
        return;
    }

    uint physicalX = g_pc.regionX + dispatchId.x;
    uint physicalY = g_pc.regionY + dispatchId.y;

    uint logicalX = WrapIndex(int(physicalX) - int(g_pc.originX), g_pc.resolution);
    uint logicalY = WrapIndex(int(physicalY) - int(g_pc.originY), g_pc.resolution);

    float halfCells = (float(g_pc.resolution) - 1.0) * 0.5;
    float2 localOffsetMeters =
        (float2(float(logicalX), float(logicalY)) - halfCells) *
        g_pc.spacingMeters;
    float2 offsetMeters =
        float2(g_pc.fineUp.w, g_pc.fineEast.w) +
        localOffsetMeters;

    float radius = ParamFloat(kParamPlanetRadiusMeters);
    float3 direction = DirectionAtSurfaceOffset(g_pc.fineUp.xyz, g_pc.fineEast.xyz, g_pc.fineNorth.xyz, offsetMeters, radius);

    FullSample sample = GenerateSample(direction, g_pc.footprintMeters);

    float fineFootprint = g_pc.fineNormalFootprintMeters > 0.0 ? g_pc.fineNormalFootprintMeters : g_pc.footprintMeters;
    float fineEpsilon = g_pc.fineNormalEpsilonMeters > 0.0 ? g_pc.fineNormalEpsilonMeters : g_pc.spacingMeters;

    float2 fineSlope = SampleFineSlope(
        g_pc.fineUp.xyz, g_pc.fineEast.xyz, g_pc.fineNorth.xyz, offsetMeters, fineFootprint, fineEpsilon, radius);

    float2 morphTarget = offsetMeters;

    if (g_pc.morphToCoarser != 0u)
    {
        // Adjacent clipmaps share one stable spherical lattice. Parent morphing
        // is therefore pure integer-grid snapping: no spherical frame round trip
        // is necessary. This is both faster and exactly phase-consistent with
        // the parent samples.
        float2 snappedCoarseOffset =
            round(offsetMeters / g_pc.coarseSpacingMeters) *
            g_pc.coarseSpacingMeters;

        float3 coarseDirection = DirectionAtSurfaceOffset(
            g_pc.fineUp.xyz,
            g_pc.fineEast.xyz,
            g_pc.fineNorth.xyz,
            snappedCoarseOffset,
            radius);

        morphTarget = snappedCoarseOffset;

        float edgeDistance =
            max(abs(localOffsetMeters.x), abs(localOffsetMeters.y));
        float normalized = saturate(
            (edgeDistance - g_pc.morphStartHalfExtentMeters) /
            max(g_pc.morphEndHalfExtentMeters - g_pc.morphStartHalfExtentMeters, 0.0001));
        float morph = normalized * normalized * (3.0 - 2.0 * normalized);

        if (morph > 0.0)
        {
            FullSample coarseSample = GenerateSample(coarseDirection, g_pc.coarseFootprintMeters);
            sample = LerpFullSample(sample, coarseSample, morph);

            float2 coarseFineSlope = SampleFineSlope(
                g_pc.fineUp.xyz, g_pc.fineEast.xyz, g_pc.fineNorth.xyz, snappedCoarseOffset,
                fineFootprint, fineEpsilon, radius);
            fineSlope = lerp(fineSlope, coarseFineSlope, morph);
        }
    }

    uint physicalIndex = physicalY * g_pc.resolution + physicalX;
    uint byteOffset = physicalIndex * kSampleStrideBytes;

    g_output.Store(byteOffset + 0u, asuint(sample.elevationMeters));
    g_output.Store(byteOffset + 4u, asuint(morphTarget.x));
    g_output.Store(byteOffset + 8u, asuint(morphTarget.y));
    g_output.Store(byteOffset + 12u, PackUnorm4x8(sample.biome0));
    g_output.Store(byteOffset + 16u, PackUnorm4x8(sample.biome1));
    g_output.Store(byteOffset + 20u, asuint(sample.standingWaterDepthMeters));
    g_output.Store(byteOffset + 24u, asuint(fineSlope.x));
    g_output.Store(byteOffset + 28u, asuint(fineSlope.y));
}
)";
} // namespace orbit::terrain_gpu::detail
