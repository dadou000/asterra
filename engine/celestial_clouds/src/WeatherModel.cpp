#include <orbit/celestial_clouds/WeatherModel.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbit::celestial_clouds
{
namespace
{
constexpr f64 kPi = std::numbers::pi_v<f64>;

[[nodiscard]] f64 Saturate(const f64 value) noexcept
{
    return std::clamp(value, 0.0, 1.0);
}

[[nodiscard]] f64 Smooth(const f64 a, const f64 b, const f64 x) noexcept
{
    const f64 t = Saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}

[[nodiscard]] f64 Bump(const f64 distance, const f64 width) noexcept
{
    const f64 t = distance / width;
    return std::exp(-t * t);
}

[[nodiscard]] f64 Hash(const i64 x, const i64 y, const i64 z, const u64 seed) noexcept
{
    u64 h = seed;
    h = terrain::StableCombine64(h, static_cast<u64>(x));
    h = terrain::StableCombine64(h, static_cast<u64>(y));
    h = terrain::StableCombine64(h, static_cast<u64>(z));
    return static_cast<f64>(h & 0xFFFFFFULL) / 16777215.0;
}

// Smooth 3D value noise in [0, 1].
[[nodiscard]] f64 ValueNoise(const math::Double3& p, const u64 seed) noexcept
{
    const f64 fx = std::floor(p.x);
    const f64 fy = std::floor(p.y);
    const f64 fz = std::floor(p.z);
    const i64 ix = static_cast<i64>(fx);
    const i64 iy = static_cast<i64>(fy);
    const i64 iz = static_cast<i64>(fz);
    const auto fade = [](const f64 t) { return t * t * (3.0 - 2.0 * t); };
    const f64 tx = fade(p.x - fx);
    const f64 ty = fade(p.y - fy);
    const f64 tz = fade(p.z - fz);
    const auto corner = [&](const i64 dx, const i64 dy, const i64 dz)
    {
        return Hash(ix + dx, iy + dy, iz + dz, seed);
    };
    const f64 x00 = corner(0, 0, 0) + (corner(1, 0, 0) - corner(0, 0, 0)) * tx;
    const f64 x10 = corner(0, 1, 0) + (corner(1, 1, 0) - corner(0, 1, 0)) * tx;
    const f64 x01 = corner(0, 0, 1) + (corner(1, 0, 1) - corner(0, 0, 1)) * tx;
    const f64 x11 = corner(0, 1, 1) + (corner(1, 1, 1) - corner(0, 1, 1)) * tx;
    const f64 y0 = x00 + (x10 - x00) * ty;
    const f64 y1 = x01 + (x11 - x01) * ty;
    return y0 + (y1 - y0) * tz;
}

[[nodiscard]] f64 Fbm(const math::Double3& p, const u64 seed, const int octaves) noexcept
{
    f64 sum = 0.0;
    f64 amplitude = 0.5;
    f64 norm = 0.0;
    math::Double3 q = p;
    for (int i = 0; i < octaves; ++i)
    {
        sum += amplitude * ValueNoise(q, seed + static_cast<u64>(i) * 0x9E3779B9ULL);
        norm += amplitude;
        q = {q.x * 2.03 + 11.7, q.y * 2.03 - 5.3, q.z * 2.03 + 3.1};
        amplitude *= 0.5;
    }
    return sum / norm;
}
} // namespace

WeatherState EvaluateWeather(
    const math::Double3& d,
    const f64 seconds,
    const WeatherParameters& parameters,
    const WeatherClimate& climate) noexcept
{
    const f64 sinLat = std::clamp(d.y, -1.0, 1.0);
    const f64 latitude = std::asin(sinLat);
    const f64 longitude = std::atan2(d.z, d.x);

    // Slow evolution: the noise field drifts through a third dimension so
    // systems grow and decay instead of only translating.
    const f64 evolve = seconds * 1.5e-5;
    const u64 seed = parameters.seed;

    // Domain warp: bend the bands so they never read as perfect latitude stripes.
    const math::Double3 warpP{d.x * 2.1 + 3.7, d.y * 2.1 - 1.9, d.z * 2.1 + evolve};
    const f64 warpLat = (Fbm(warpP, seed ^ 0xA11CE5ULL, 3) - 0.5) * 0.30;
    const f64 warpLon = (Fbm(warpP + math::Double3{9.3, 4.1, -2.7}, seed ^ 0xB0B5ULL, 3) - 0.5) * 0.9;
    const f64 lat = latitude + warpLat;
    const f64 lon = longitude + warpLon;
    const f64 absLat = std::abs(lat);

    // ---- Tropics: convergence zone with organised deep convection -------------
    const f64 itczLatitude = 0.45 * parameters.subsolarLatitudeRadians;
    const f64 itcz = Bump(lat - itczLatitude, 0.13);
    const auto clusterAt = [&](const math::Double3& q)
    {
        return Smooth(
            0.42, 0.72,
            Fbm(q * (parameters.noiseScale * 1.6) + math::Double3{evolve, 0.0, 7.7}, seed ^ 0xC1u, 4));
    };
    const f64 cluster = clusterAt(d);
    const f64 tropicalCover = itcz * (0.18 + 0.82 * cluster);

    // ---- Subtropics: subsidence suppresses cloud, shallow trade cumulus ----------
    const f64 subsidence = Bump(absLat - 0.47, 0.17);
    const f64 tradeBand = Smooth(0.10, 0.22, absLat) * (1.0 - Smooth(0.42, 0.62, absLat));
    const f64 tradeNoise = Fbm(d * (parameters.detailScale * 0.9) + math::Double3{0.0, evolve * 2.0, 1.3}, seed ^ 0x7Au, 4);
    const f64 tradeCover = tradeBand * Smooth(0.52, 0.78, tradeNoise) * 0.55;

    // ---- Mid-latitude storm tracks with baroclinic waves -------------------------
    // Two wave trains (wavenumbers around 5 and 7) are blended by a slowly
    // varying noise so cyclones differ in size, spacing and strength.
    f64 frontal = 0.0;
    f64 cyclonic = 0.0;
    f64 shield = 0.0;
    f64 jetCirrus = 0.0;
    for (int hemisphere = -1; hemisphere <= 1; hemisphere += 2)
    {
        const f64 sign = static_cast<f64>(hemisphere);
        const f64 trackLatitude =
            sign * 0.90 + 0.18 * parameters.subsolarLatitudeRadians;
        const f64 trainBlend = Fbm(
            d * 1.3 + math::Double3{sign * 5.0, evolve, 2.0}, seed ^ 0xBEEFu, 3);
        const f64 waveNumber =
            parameters.stormWaveNumber - 1.0 + 2.0 * trainBlend;
        const f64 phase = waveNumber * lon + (sign > 0.0 ? 0.7 : 2.9) + evolve * 6.0;
        const f64 meander = 0.14 * std::sin(phase) + 0.06 * std::sin(2.3 * phase + 1.1);
        const f64 distance = lat - (trackLatitude + meander);
        const f64 band = Bump(distance, 0.15);
        // How strong this particular cyclone is, from noise along the track.
        const f64 vigor = 0.35 + 1.1 * Fbm(
            d * 2.2 + math::Double3{sign * 3.0, 1.0, -evolve}, seed ^ 0x51C0u, 3);
        const f64 trough = (0.5 + 0.5 * std::sin(phase + 1.0)) * std::min(vigor, 1.2);
        // Trailing cold front and warm sector, plus a broad stratiform shield.
        const f64 sheet = band * (0.22 + 0.78 * trough);
        const f64 headDistance = lat - (trackLatitude + meander + sign * 0.11);
        const f64 head = Bump(headDistance, 0.10) * Smooth(0.45, 0.9, trough);
        const f64 shieldBand = Bump(distance - sign * 0.05, 0.26) * trough * 0.55;
        // Cirrus streaks fan out ahead of the warm front along the jet, poleward
        // of the track and east of the trough.
        const f64 ahead = Smooth(0.25, 0.85, 0.5 + 0.5 * std::sin(phase + 2.3));
        jetCirrus = std::max(
            jetCirrus,
            Bump(lat - (trackLatitude + meander + sign * 0.05), 0.09) * ahead * (0.35 + 0.65 * std::min(vigor, 1.0)));
        frontal = std::max(frontal, sheet);
        cyclonic = std::max(cyclonic, head);
        shield = std::max(shield, shieldBand);
    }
    const f64 stormNoise = Fbm(d * (parameters.noiseScale * 2.4) + math::Double3{2.2, evolve * 3.0, -4.4}, seed ^ 0x5701u, 4);
    const f64 stormCover = Saturate(std::max(frontal * (0.8 + 0.5 * stormNoise), shield) + 0.9 * cyclonic);

    // ---- Layered cloud: stratocumulus decks and stratiform sheets ------------------
    const f64 layerNoise = Fbm(d * (parameters.noiseScale * 1.4) + math::Double3{evolve, 3.3, 0.9}, seed ^ 0x1A7Eu, 4);
    const f64 layerEnvelope = Smooth(0.12, 0.35, absLat) * (1.0 - 0.5 * Smooth(1.1, 1.4, absLat));
    const f64 layered = layerEnvelope * Smooth(0.42, 0.66, layerNoise) * 0.6;

    // ---- Polar low stratus -------------------------------------------------------
    const f64 polarCover = Smooth(1.15, 1.40, absLat) * (0.25 + 0.5 * Fbm(d * 5.0, seed ^ 0x90u, 3));

    // ---- High cloud: anvils, jet cirrus and thin tropical cirrus -----------------
    // Anvil outflow: deep convection upwind (to the east) spreads cirrus
    // downwind, so sample the convective clusters at a few upwind longitudes.
    f64 anvil = 0.0;
    if (itcz > 0.02)
    {
        for (int k = 0; k <= 4; ++k)
        {
            const f64 angle = static_cast<f64>(k) * 0.014;
            const f64 c = std::cos(angle);
            const f64 sn = std::sin(angle);
            const math::Double3 upwind{d.x * c + d.z * sn, d.y, -d.x * sn + d.z * c};
            anvil = std::max(anvil, clusterAt(upwind) * (1.0 - 0.2 * static_cast<f64>(k)));
        }
        anvil *= itcz;
    }
    const f64 tropicalHigh =
        0.30 * Bump(lat - itczLatitude, 0.32) *
        Smooth(0.5, 0.74, Fbm(d * (parameters.noiseScale * 2.0) + math::Double3{0.0, 4.4, evolve}, seed ^ 0xC1C1u, 3));
    const f64 jetStrength = Smooth(0.2, 0.6, Fbm(d * (parameters.noiseScale * 1.8) + math::Double3{6.1, evolve, 0.0}, seed ^ 0x1E7u, 3));
    f64 cirrus = std::max({anvil * 0.95, jetCirrus * (0.55 + 0.45 * jetStrength), tropicalHigh});

    // ---- Combine and modulate by the climate authority ---------------------------
    f64 coverage = tropicalCover + tradeCover + stormCover + polarCover + layered;
    coverage *= 1.0 - 0.55 * subsidence * (1.0 - Smooth(0.5, 0.9, tropicalCover));

    f64 humidity = 0.5;
    f64 rain = 0.35;
    f64 warmth = 0.0;
    if (climate.valid)
    {
        humidity = Saturate(climate.humidity);
        rain = Saturate(climate.precipitation);
        warmth = std::clamp((climate.temperatureC - 15.0) / 25.0, -1.0, 1.0);
    }
    coverage *= 0.55 + 0.9 * humidity;
    coverage += 0.35 * rain * Smooth(0.2, 0.7, rain);
    // Fine breakup so the large systems have ragged, broken edges.
    const f64 fine = Fbm(d * parameters.detailScale + math::Double3{evolve * 4.0, 5.0, 0.0}, seed ^ 0xF1u, 4);
    coverage *= 0.7 + 0.6 * fine;
    coverage = Saturate(coverage * 1.05);

    // ---- Cloud type: weighted by what produced the cloud ---------------------------
    const f64 wDeep = tropicalCover * 1.2 + 0.3 * rain * warmth;
    const f64 wCumulus = tradeCover;
    const f64 wFrontal = stormCover;
    const f64 wPolar = polarCover + layered * 0.8;
    const f64 total = std::max(wDeep + wCumulus + wFrontal + wPolar, 1.0e-6);
    // Deep tropical cores reach cumulonimbus, trades are shallow cumulus,
    // fronts are layered (stratus / nimbostratus) with a taller cyclone head.
    const f64 frontalType = 0.08 + 0.26 * Saturate(cyclonic * 1.6);
    f64 cloudType =
        (wDeep * (0.72 + 0.28 * Smooth(0.5, 0.95, tropicalCover)) +
         wCumulus * 0.5 + wFrontal * frontalType + polarCover * 0.05 + layered * 0.8 * 0.2) /
        total;
    cloudType = Saturate(cloudType + 0.10 * warmth * Smooth(0.3, 0.8, rain));

    f64 precipitation =
        0.85 * std::pow(Saturate(tropicalCover), 1.4) * Smooth(0.6, 1.0, cloudType) +
        0.7 * Saturate(frontal * (0.4 + cyclonic)) +
        0.5 * rain * coverage;
    precipitation = Saturate(precipitation) * Smooth(0.25, 0.7, coverage);

    cirrus = Saturate(cirrus * (0.6 + 0.8 * humidity));
    return {
        .coverage = coverage,
        .cloudType = cloudType,
        .cirrus = cirrus,
        .precipitation = precipitation};
}
} // namespace orbit::celestial_clouds
