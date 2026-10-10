#include <orbit/weather_lab/CloudLayers.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>

namespace orbit::weather_lab
{
namespace
{
struct Vec3
{
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
};

[[nodiscard]] float Fract(const float value) noexcept
{
    return value - std::floor(value);
}

[[nodiscard]] float Saturate(const float value) noexcept
{
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] float SmoothStep(const float edge0, const float edge1, const float value) noexcept
{
    const float t = Saturate((value - edge0) / (edge1 - edge0));
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] float Hash(const std::int32_t x, const std::int32_t y, const std::int32_t z, const std::uint32_t seed) noexcept
{
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343U ^
        static_cast<std::uint32_t>(y) * 0xd8163841U ^
        static_cast<std::uint32_t>(z) * 0xcb1ab31fU ^ seed * 0x9e3779b9U;
    h ^= h >> 16U;
    h *= 0x7feb352dU;
    h ^= h >> 15U;
    h *= 0x846ca68bU;
    h ^= h >> 16U;
    return static_cast<float>(h & 0xffffffU) / static_cast<float>(0x1000000U);
}

[[nodiscard]] float ValueNoise(const Vec3 p, const std::uint32_t seed) noexcept
{
    const float fx = std::floor(p.x);
    const float fy = std::floor(p.y);
    const float fz = std::floor(p.z);
    const auto ix = static_cast<std::int32_t>(fx);
    const auto iy = static_cast<std::int32_t>(fy);
    const auto iz = static_cast<std::int32_t>(fz);
    float tx = p.x - fx;
    float ty = p.y - fy;
    float tz = p.z - fz;
    tx = tx * tx * (3.0F - 2.0F * tx);
    ty = ty * ty * (3.0F - 2.0F * ty);
    tz = tz * tz * (3.0F - 2.0F * tz);
    const auto lerp = [](const float a, const float b, const float t) { return a + (b - a) * t; };
    const float c00 = lerp(Hash(ix, iy, iz, seed), Hash(ix + 1, iy, iz, seed), tx);
    const float c10 = lerp(Hash(ix, iy + 1, iz, seed), Hash(ix + 1, iy + 1, iz, seed), tx);
    const float c01 = lerp(Hash(ix, iy, iz + 1, seed), Hash(ix + 1, iy, iz + 1, seed), tx);
    const float c11 = lerp(Hash(ix, iy + 1, iz + 1, seed), Hash(ix + 1, iy + 1, iz + 1, seed), tx);
    return lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz);
}

// Fractal value noise in [0, 1].
[[nodiscard]] float Fbm(Vec3 p, const std::uint32_t seed, const int octaves) noexcept
{
    float sum = 0.0F;
    float amplitude = 0.5F;
    float norm = 0.0F;
    for (int octave = 0; octave < octaves; ++octave)
    {
        sum += amplitude * ValueNoise(p, seed + static_cast<std::uint32_t>(octave) * 31U);
        norm += amplitude;
        amplitude *= 0.5F;
        p = {p.x * 2.03F + 17.0F, p.y * 2.03F + 5.0F, p.z * 2.03F + 11.0F};
    }
    return sum / norm;
}

// Distance to the nearest feature point in a jittered grid (cellular noise):
// 0 at a point, about 1 at the cell edge.
[[nodiscard]] float CellDistance(const Vec3 p, const std::uint32_t seed, float& cellRandom) noexcept
{
    const auto ix = static_cast<std::int32_t>(std::floor(p.x));
    const auto iy = static_cast<std::int32_t>(std::floor(p.y));
    float best = 8.0F;
    cellRandom = 0.0F;
    for (std::int32_t dy = -1; dy <= 1; ++dy)
    {
        for (std::int32_t dx = -1; dx <= 1; ++dx)
        {
            const std::int32_t cx = ix + dx;
            const std::int32_t cy = iy + dy;
            const float px = static_cast<float>(cx) + Hash(cx, cy, 0, seed);
            const float py = static_cast<float>(cy) + Hash(cx, cy, 1, seed);
            const float ddx = px - p.x;
            const float ddy = py - p.y;
            const float distance = std::sqrt(ddx * ddx + ddy * ddy);
            if (distance < best)
            {
                best = distance;
                cellRandom = Hash(cx, cy, 2, seed);
            }
        }
    }
    return best;
}

// Nearest feature point of a jittered grid: its distance, a random value, the
// integer cell and the vector from the feature point to p (cell units).
struct NearestCell
{
    float distance{8.0F};
    float random{0.0F};
    std::int32_t cx{0};
    std::int32_t cy{0};
    float dx{0.0F};
    float dy{0.0F};
};

[[nodiscard]] NearestCell FindNearestCell(const Vec3 p, const std::uint32_t seed) noexcept
{
    const auto ix = static_cast<std::int32_t>(std::floor(p.x));
    const auto iy = static_cast<std::int32_t>(std::floor(p.y));
    NearestCell best;
    for (std::int32_t dy = -1; dy <= 1; ++dy)
    {
        for (std::int32_t dx = -1; dx <= 1; ++dx)
        {
            const std::int32_t cx = ix + dx;
            const std::int32_t cy = iy + dy;
            const float px = static_cast<float>(cx) + Hash(cx, cy, 0, seed);
            const float py = static_cast<float>(cy) + Hash(cx, cy, 1, seed);
            const float ddx = p.x - px;
            const float ddy = p.y - py;
            const float distance = std::sqrt(ddx * ddx + ddy * ddy);
            if (distance < best.distance)
            {
                best = {distance, Hash(cx, cy, 2, seed), cx, cy, ddx, ddy};
            }
        }
    }
    return best;
}
} // namespace

std::string_view CloudLayerKindName(const CloudLayerKind kind) noexcept
{
    return cloud_genera::CloudGenusName(kind);
}

bool ParseCloudLayerKind(const std::string_view name, CloudLayerKind& kind) noexcept
{
    return cloud_genera::ParseCloudGenus(name, kind);
}

bool IsGeneratedCloudLayer(const CloudLayerKind kind) noexcept
{
    return !cloud_genera::GenusInfo(kind).simulated;
}

CloudLayerPlacement DefaultCloudLayerPlacement(const CloudLayerKind kind) noexcept
{
    const auto& info = cloud_genera::GenusInfo(kind);
    return {
        0.5 * (info.baseMeters + info.topMeters),
        0.5 * (info.topMeters - info.baseMeters),
        info.extinctionScale,
        info.recommendedHalfExtentMeters};
}

namespace
{
// Everything a genus' density function needs at one voxel.
struct Sample
{
    float x{0.0F};
    float y{0.0F};
    float z{0.0F};
    float h{0.0F};      // height in the layer, -1 (bottom) .. +1 (top)
    float along{0.0F};  // metres along the wind
    float across{0.0F}; // metres across the wind
    float coverage{0.5F};
    std::uint32_t seed{1U};
    std::uint32_t style{0U};
    float halfZ{1000.0F};
};

// Cirrus (photos: hair-like strands and hooks, translucent, dense in places
// and fading into nothing at the tail): fibres drawn out along the wind with
// a slow sideways meander, in patches of tens of kilometres.
float Cirrus(const Sample& s)
{
    const float meander = (Fbm({s.along / 21000.0F, s.across / 9000.0F, 8.0F}, s.seed + 71U, 3) - 0.5F) * 6200.0F;
    // Style bit 0: hooked strands, the heads curl away from the wind.
    const float hook = (s.style & 1U) != 0U
        ? 2600.0F * std::sin(s.along / 7500.0F + 6.0F * Fbm({s.x / 16000.0F, s.y / 16000.0F, 1.0F}, s.seed + 74U, 2))
        : 0.0F;
    const float across = s.across + meander + hook;
    const float fineness = (s.style & 4U) != 0U ? 0.5F : 1.0F;
    const float along = s.along + s.z * 0.35F;
    const float patch = Fbm({along / 26000.0F, across / 21000.0F, 3.0F}, s.seed + 11U, 4);
    const float mask = SmoothStep(0.62F - 0.3F * s.coverage, 0.78F - 0.3F * s.coverage, patch);
    const float fibre =
        0.50F * ValueNoise({along / 11000.0F, across * fineness / 900.0F, s.z / 500.0F}, s.seed + 3U) +
        0.30F * ValueNoise({along / 6000.0F + 7.0F, across * fineness / 420.0F, s.z / 300.0F}, s.seed + 5U) +
        0.20F * ValueNoise({along / 2600.0F + 3.0F, across * fineness / 170.0F, s.z / 200.0F}, s.seed + 9U);
    // Style bit 1: dense bundles (a wider band of the noise counts as strand).
    const float strands = (s.style & 2U) != 0U ? SmoothStep(0.28F, 0.62F, fibre) : SmoothStep(0.36F, 0.74F, fibre);
    // Heads are denser than the tails they trail.
    const float tail = 0.45F + 0.55F * Fbm({along / 15000.0F, across / 6000.0F, 5.0F}, s.seed + 13U, 2);
    return 0.5F * mask * strands * tail * std::exp(-s.h * s.h * 3.2F);
}

// Cirrocumulus (photos: tiny white grains in tidy rows, gaps of blue between,
// almost no shading): a thin sheet of cells a few hundred metres wide,
// organised into ripples across the wind.
float Cirrocumulus(const Sample& s)
{
    const float patch = SmoothStep(0.5F - 0.32F * s.coverage, 0.66F - 0.32F * s.coverage,
        Fbm({s.x / 9000.0F, s.y / 9000.0F, 2.0F}, s.seed + 81U, 3));
    // Ripples: rows across the wind that bend slowly.
    const float bend = (Fbm({s.x / 7000.0F, s.y / 7000.0F, 4.0F}, s.seed + 82U, 2) - 0.5F) * 6.0F;
    const float rows = 0.5F + 0.5F * std::sin(s.along / 560.0F * 6.2831853F * 0.5F + bend);
    float cellRandom = 0.0F;
    const float distance = CellDistance({s.along / 430.0F, s.across / 430.0F, 0.0F}, s.seed + 83U, cellRandom);
    const float grain = SmoothStep(0.58F, 0.22F, distance) * (0.55F + 0.45F * cellRandom) * (0.45F + 0.55F * rows);
    const float profile = std::exp(-s.h * s.h * 3.0F);
    return 0.75F * patch * grain * profile;
}

// Cirrostratus (photos: a milky, nearly featureless veil with the faintest
// fibre streaks; blue sky and the sun shine through it): thin, even and wide.
float Cirrostratus(const Sample& s)
{
    const float veil = Fbm({s.x / 38000.0F, s.y / 38000.0F, 1.0F}, s.seed + 21U, 4);
    const float mask = SmoothStep(0.5F - 0.4F * s.coverage, 0.78F - 0.4F * s.coverage, veil);
    const float streak = 0.82F + 0.36F * ValueNoise({s.along / 14000.0F, s.across / 1500.0F, 2.0F}, s.seed + 22U);
    const float grain = 0.88F + 0.24F * ValueNoise({s.x / 2600.0F, s.y / 2600.0F, s.z / 500.0F}, s.seed + 4U);
    return 0.2F * mask * streak * grain * std::exp(-s.h * s.h * 2.0F);
}

// Altocumulus (photos: rounded cloudlets one to five degrees wide in rows or
// patches, bright tops, grey undersides, clear gaps): cells of about a
// kilometre grouped into wave patches.
float Altocumulus(const Sample& s)
{
    const float patch = SmoothStep(0.46F - 0.3F * s.coverage, 0.64F - 0.3F * s.coverage,
        Fbm({s.x / 14000.0F, s.y / 14000.0F, 2.0F}, s.seed + 31U, 4));
    // Wave rows: the cells line up across the wind.
    const float bend = (Fbm({s.x / 9000.0F, s.y / 9000.0F, 7.0F}, s.seed + 32U, 2) - 0.5F) * 5.0F;
    const float rows = 0.5F + 0.5F * std::sin(s.along / 1900.0F * 6.2831853F + bend);
    float cellRandom = 0.0F;
    const float distance = CellDistance({s.along / 1050.0F, s.across / 720.0F, 0.0F}, s.seed + 6U, cellRandom);
    const float puff = SmoothStep(0.64F, 0.2F, distance) * (0.55F + 0.45F * cellRandom) * (0.4F + 0.6F * rows);
    const float top = -0.1F + 0.7F * cellRandom;
    const float profile = SmoothStep(-1.0F, -0.6F, s.h) * (1.0F - SmoothStep(top - 0.35F, top + 0.2F, s.h));
    const float lobes = 0.7F + 0.6F * ValueNoise({s.x / 380.0F, s.y / 380.0F, s.z / 260.0F}, s.seed + 8U);
    return 0.9F * patch * puff * profile * lobes;
}

// Altostratus (photos: a featureless grey sheet, darker toward the horizon,
// the sun a dim disc behind it, faint streaks): wide, smooth, uneven in
// thickness, no cells.
float Altostratus(const Sample& s)
{
    const float sheet = Fbm({s.x / 42000.0F, s.y / 42000.0F, 4.0F}, s.seed + 41U, 4);
    const float mask = SmoothStep(0.38F - 0.3F * s.coverage, 0.62F - 0.3F * s.coverage, sheet);
    const float streak = 0.8F + 0.4F * ValueNoise({s.along / 22000.0F, s.across / 3200.0F, 3.0F}, s.seed + 43U);
    const float grain = 0.8F + 0.4F * Fbm({s.x / 7000.0F, s.y / 7000.0F, s.z / 1200.0F}, s.seed + 12U, 3);
    return 0.55F * mask * streak * grain * std::exp(-s.h * s.h * 2.0F);
}

// Nimbostratus (photos: a deep, dark, uniform deck; ragged low scud hangs
// under it; no sun at all): a thick body with a tattered base.
float Nimbostratus(const Sample& s)
{
    const float sheet = Fbm({s.x / 52000.0F, s.y / 52000.0F, 6.0F}, s.seed + 51U, 4);
    const float mask = SmoothStep(0.34F - 0.28F * s.coverage, 0.52F - 0.28F * s.coverage, sheet);
    const float body = SmoothStep(-0.55F, -0.3F, s.h) * (1.0F - SmoothStep(0.62F, 0.95F, s.h));
    // Scud: shreds below the body, thickest where rain falls.
    const float shreds = Fbm({s.x / 1900.0F, s.y / 1900.0F, s.z / 420.0F}, s.seed + 52U, 4);
    const float scud = SmoothStep(0.5F, 0.72F, shreds) * SmoothStep(-1.0F, -0.8F, s.h) *
        (1.0F - SmoothStep(-0.62F, -0.3F, s.h));
    const float mottle = 0.8F + 0.4F * Fbm({s.x / 9000.0F, s.y / 9000.0F, s.z / 1600.0F}, s.seed + 53U, 3);
    return Saturate(mask * (0.9F * body * mottle + 0.55F * scud));
}

// Stratocumulus (photos: big soft lumps with dark undersides and bright
// rims, joined into a deck with gaps, in rolls along the wind): closed cells
// a few kilometres across, flat based, lumpy topped.
float Stratocumulus(const Sample& s)
{
    const float rolls = Fbm({s.along / 12000.0F, s.across / 4800.0F, 1.0F}, s.seed + 61U, 3);
    const float deck = SmoothStep(0.44F - 0.3F * s.coverage, 0.62F - 0.3F * s.coverage, rolls);
    float cellRandom = 0.0F;
    const float warpX = (Fbm({s.x / 3500.0F, s.y / 3500.0F, 3.0F}, s.seed + 62U, 2) - 0.5F) * 1.4F;
    const float distance = CellDistance(
        {s.along / 3300.0F + warpX, s.across / 2500.0F - warpX, 0.0F}, s.seed + 63U, cellRandom);
    const float clump = SmoothStep(0.68F, 0.22F, distance);
    const float top = -0.25F + 0.8F * cellRandom;
    const float profile = SmoothStep(-1.0F, -0.72F, s.h) * (1.0F - SmoothStep(top - 0.35F, top + 0.25F, s.h));
    const float lumps = Fbm({s.x / 1100.0F, s.y / 1100.0F, s.z / 420.0F}, s.seed + 14U, 4);
    return Saturate(deck * clump * profile * (0.45F + 1.1F * lumps));
}

// Stratus (photos: a smooth, low, uniform grey sheet, soft and slightly
// mottled, a ragged hazy base): thin and wide, no structure to speak of.
float Stratus(const Sample& s)
{
    const float sheet = Fbm({s.x / 30000.0F, s.y / 30000.0F, 9.0F}, s.seed + 71U, 4);
    const float mask = SmoothStep(0.36F - 0.3F * s.coverage, 0.56F - 0.3F * s.coverage, sheet);
    const float mottle = 0.75F + 0.5F * Fbm({s.x / 4200.0F, s.y / 4200.0F, s.z / 400.0F}, s.seed + 72U, 4);
    const float base = SmoothStep(-1.0F, -0.35F + 0.5F * Fbm({s.x / 1500.0F, s.y / 1500.0F, 2.0F}, s.seed + 73U, 3), s.h);
    return 0.75F * mask * mottle * base * (1.0F - SmoothStep(0.55F, 1.0F, s.h));
}

// Cumulus (photos: bright white cauliflower heads with crisp edges and flat
// grey bases, separated by blue sky): each cloud is a core with a crown of
// overlapping lobes, so the outline is a cauliflower at the scale the grid can
// hold (hundreds of metres) before the shader adds the fine billows.
float Cumulus(const Sample& s)
{
    const float cell = 4200.0F;
    const NearestCell nearest = FindNearestCell({s.x / cell, s.y / cell, 0.0F}, s.seed + 7U);
    if (nearest.random >= 0.2F + 0.6F * s.coverage)
    {
        return 0.0F;
    }
    const bool moreLobes = (s.style & 1U) != 0U;
    const bool towering = (s.style & 2U) != 0U;
    const bool sharp = (s.style & 4U) != 0U;
    const float radius = (0.24F + 0.16F * nearest.random) * cell;
    const float lx = nearest.dx * cell;
    const float ly = nearest.dy * cell;
    if (std::sqrt(lx * lx + ly * ly) > radius * 1.5F)
    {
        return 0.0F;
    }
    const float base = -s.halfZ + 0.05F * s.halfZ;
    const float tall = (0.5F + 0.5F * nearest.random) * 2.0F * s.halfZ * (towering ? 1.0F : 0.7F);
    const float zRel = s.z - base; // metres above the cloud base

    // Core: a flattened ellipsoid that holds the body together.
    const float coreR = radius * 0.62F;
    const float coreH = tall * 0.55F;
    const float coreD = std::sqrt((lx * lx + ly * ly) / (coreR * coreR) + (zRel - coreH * 0.5F) * (zRel - coreH * 0.5F) / (coreH * coreH * 0.25F));
    float value = SmoothStep(1.0F, 0.1F, coreD);

    // Lobes: spheres piled toward the top, smaller and brighter as they rise.
    const int count = moreLobes ? 9 : 5;
    for (int i = 0; i < count; ++i)
    {
        const float a = Hash(nearest.cx * 7 + i, nearest.cy, 10 + i, s.seed) * 6.2831853F;
        const float spread = std::sqrt(Hash(nearest.cx * 7 + i, nearest.cy, 30 + i, s.seed)) * radius * 0.62F;
        const float rise = 0.3F + 0.62F * Hash(nearest.cx * 7 + i, nearest.cy, 50 + i, s.seed);
        const float lobeR = radius * (moreLobes ? 0.22F : 0.3F) * (1.15F - 0.5F * rise) *
            (0.8F + 0.4F * Hash(nearest.cx * 7 + i, nearest.cy, 70 + i, s.seed));
        const float cx = std::cos(a) * spread * (1.0F - 0.55F * rise);
        const float cy = std::sin(a) * spread * (1.0F - 0.55F * rise);
        const float cz = tall * rise * 0.85F;
        const float d = std::sqrt((lx - cx) * (lx - cx) + (ly - cy) * (ly - cy) + (zRel - cz) * (zRel - cz)) / lobeR;
        // Smooth union: overlapping lobes blend instead of leaving creases on the voxel grid.
        const float lobe = SmoothStep(1.0F, sharp ? 0.45F : 0.05F, d);
        value = 1.0F - (1.0F - value) * (1.0F - lobe);
    }

    // Flat base, then fine billows eat the outline from outside in.
    value *= SmoothStep(0.0F, 0.06F * tall + 20.0F, zRel);
    const float billow = Fbm({s.x / 380.0F, s.y / 380.0F, s.z / 300.0F}, s.seed + 16U, 3);
    return Saturate((value - (1.0F - billow) * (sharp ? 0.34F : 0.22F)) * 1.7F);
}
} // namespace

std::vector<float> GenerateCloudLayerDensity(const CloudLayerSettings& settings)
{
    const std::uint32_t nx = std::max(settings.resolutionX, 1U);
    const std::uint32_t ny = std::max(settings.resolutionY, 1U);
    const std::uint32_t nz = std::max(settings.resolutionZ, 1U);
    if (!IsGeneratedCloudLayer(settings.kind))
    {
        return {};
    }
    std::vector<float> density(static_cast<std::size_t>(nx) * ny * nz, 0.0F);

    const float windLength = std::max(std::sqrt(settings.windX * settings.windX + settings.windY * settings.windY), 1.0e-4F);
    const float windX = settings.windX / windLength;
    const float windY = settings.windY / windLength;
    const float amount = std::max(settings.density, 0.0F);
    const float halfZ = static_cast<float>(settings.halfExtentZ);

    Sample s;
    s.coverage = Saturate(settings.coverage);
    s.seed = settings.seed;
    s.style = settings.style;
    s.halfZ = halfZ;

    for (std::uint32_t k = 0U; k < nz; ++k)
    {
        s.h = ((static_cast<float>(k) + 0.5F) / static_cast<float>(nz)) * 2.0F - 1.0F;
        s.z = s.h * halfZ;
        for (std::uint32_t j = 0U; j < ny; ++j)
        {
            s.y = ((static_cast<float>(j) + 0.5F) / static_cast<float>(ny) * 2.0F - 1.0F) *
                static_cast<float>(settings.halfExtentY);
            for (std::uint32_t i = 0U; i < nx; ++i)
            {
                s.x = ((static_cast<float>(i) + 0.5F) / static_cast<float>(nx) * 2.0F - 1.0F) *
                    static_cast<float>(settings.halfExtentX);
                s.along = s.x * windX + s.y * windY;
                s.across = -s.x * windY + s.y * windX;

                float value = 0.0F;
                switch (settings.kind)
                {
                case CloudLayerKind::Cirrus: value = Cirrus(s); break;
                case CloudLayerKind::Cirrocumulus: value = Cirrocumulus(s); break;
                case CloudLayerKind::Cirrostratus: value = Cirrostratus(s); break;
                case CloudLayerKind::Altocumulus: value = Altocumulus(s); break;
                case CloudLayerKind::Altostratus: value = Altostratus(s); break;
                case CloudLayerKind::Nimbostratus: value = Nimbostratus(s); break;
                case CloudLayerKind::Stratocumulus: value = Stratocumulus(s); break;
                case CloudLayerKind::Stratus: value = Stratus(s); break;
                case CloudLayerKind::Cumulus: value = Cumulus(s); break;
                case CloudLayerKind::Cumulonimbus: break;
                }

                // Fade out toward the box sides so the layer has no straight edge.
                const float edge = std::max(std::abs(s.x) / static_cast<float>(settings.halfExtentX),
                    std::abs(s.y) / static_cast<float>(settings.halfExtentY));
                value *= SmoothStep(1.0F, 0.78F, edge);
                density[(static_cast<std::size_t>(k) * ny + j) * nx + i] = Saturate(value * amount);
            }
        }
    }

    return density;
}
} // namespace orbit::weather_lab
