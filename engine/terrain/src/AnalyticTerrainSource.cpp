#include <orbit/terrain/AnalyticTerrainSource.hpp>

#include "ProceduralNoise.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace orbit::terrain
{
namespace
{
constexpr f64 kMeanRidgeSignal = 0.5;

[[nodiscard]] f64 ShapeRidge(const f64 ridge) noexcept
{
    return ridge + 0.85 * ridge * (1.0 - ridge);
}

[[nodiscard]] GlobalTerrainFieldDesc
ResolveGlobalDesc(
    const AnalyticTerrainDesc& desc) noexcept
{
    GlobalTerrainFieldDesc global =
        desc.global;

    if (global.seed == 0)
    {
        global.seed =
            desc.seed ^
            0xA57E22A6C0A57E22ULL;
    }

    return global;
}
} // namespace

AnalyticTerrainSource::AnalyticTerrainSource(
    const world::PlanetDefinition planet,
    const AnalyticTerrainDesc desc)
    : planet_(planet),
      desc_(desc),
      globalFields_(planet, ResolveGlobalDesc(desc))
{
    for (const f64 value : {planet.radiusMeters, desc.macroWavelengthMeters,
             desc.detailWavelengthMeters, desc.mountains.wavelengthMeters,
             desc.mountains.warpWavelengthMeters, desc.maximumElevationAboveSeaLevelMeters,
             desc.global.continentalWavelengthMeters, desc.global.mountainWavelengthMeters,
             desc.global.climateWavelengthMeters})
    {
        if (!std::isfinite(value) || value <= 0.0)
        {
            throw std::invalid_argument("Orbit terrain wavelengths, radius and height limit must be finite and positive.");
        }
    }
    for (const f64 value : {desc.macroAmplitudeMeters, desc.detailAmplitudeMeters,
             desc.mountains.reliefMeters, desc.mountains.warpAmplitudeMeters,
             desc.global.continentalAmplitudeMeters, desc.global.mountainAmplitudeMeters})
    {
        if (!std::isfinite(value) || value < 0.0)
        {
            throw std::invalid_argument("Orbit terrain amplitudes must be finite and nonnegative.");
        }
    }
    if (desc.detailOctaves > detailBands_.size() ||
        desc.mountains.octaves > mountainBands_.size())
    {
        throw std::invalid_argument("Orbit terrain supports at most 16 octaves per detail band.");
    }

    const auto prepare = [&](auto& bands, const u32 count, f64 wavelength,
                             f64 amplitude, const f64 lacunarity, const u64 salt)
    {
        f64 amplitudeSum = 0.0;
        for (u32 i = 0; i < count; ++i)
        {
            bands[i] = {
                .frequency = planet.radiusMeters / wavelength,
                .wavelengthMeters = wavelength,
                .amplitude = amplitude,
                .seed = (desc.seed ^ salt) + (static_cast<u64>(i) + 1U) * 0x9E3779B97F4A7C15ULL
            };
            amplitudeSum += amplitude;
            wavelength /= lacunarity;
            amplitude *= 0.5;
        }
        return amplitudeSum;
    };
    static_cast<void>(prepare(detailBands_, desc.detailOctaves,
        desc.detailWavelengthMeters, desc.detailAmplitudeMeters, 2.0, 0));
    mountainNormalization_ = std::max(prepare(mountainBands_, desc.mountains.octaves,
        desc.mountains.wavelengthMeters, 1.0, 2.03, 0xDB4F0B9175AE2165ULL), 1.0);
    warpFrequency_ = planet.radiusMeters / desc.mountains.warpWavelengthMeters;
    // Each vector-noise Jacobian entry is bounded by 2 * max(quintic fade')
    // = 3.75. Its spectral norm is at most 11.25. Account for the warp's
    // coordinate stretch before deciding which ridge octaves are resolvable.
    warpFootprintScale_ = 1.0 + 11.25 * desc.mountains.warpAmplitudeMeters /
        desc.mountains.warpWavelengthMeters;

    // Immutable recipe fingerprint, including the generator algorithm version.
    revision_ = detail::Mix64(desc.seed ^ 0x4153544552524103ULL);
    const auto mix = [&](const u64 value) { revision_ = detail::Mix64(revision_ ^ value); };
    mix(desc.global.seed);
    mix(desc.detailOctaves);
    mix(desc.mountains.octaves);
    for (const f64 value : {planet.radiusMeters, desc.macroAmplitudeMeters,
             desc.macroWavelengthMeters, desc.detailAmplitudeMeters, desc.detailWavelengthMeters,
             desc.mountains.reliefMeters, desc.mountains.wavelengthMeters,
             desc.mountains.warpWavelengthMeters, desc.mountains.warpAmplitudeMeters,
             desc.maximumElevationAboveSeaLevelMeters, desc.global.seaLevelMeters,
             desc.global.continentalAmplitudeMeters, desc.global.continentalWavelengthMeters,
             desc.global.continentalBiasMeters, desc.global.mountainAmplitudeMeters,
             desc.global.mountainWavelengthMeters, desc.global.climateWavelengthMeters,
             desc.global.equatorTemperatureC, desc.global.poleTemperatureC,
             desc.global.temperatureVariationC, desc.global.lapseRateCPerKilometer})
    {
        if (!std::isfinite(value))
        {
            throw std::invalid_argument("Orbit terrain recipe must contain finite values.");
        }
        mix(std::bit_cast<u64>(value));
    }
}

f64 AnalyticTerrainSource::LimitElevation(const f64 elevationMeters) const noexcept
{
    const f64 limit = desc_.maximumElevationAboveSeaLevelMeters;
    const f64 shoulder = limit * 0.75;
    const f64 relative = elevationMeters - desc_.global.seaLevelMeters;
    if (relative <= shoulder)
    {
        return elevationMeters;
    }
    const f64 headroom = limit - shoulder;
    const f64 x = (relative - shoulder) / headroom;
    // x(1+x)/(1+x+x*x) joins the identity with matching first and second
    // derivatives at zero and approaches one without a flat hard-clamp shelf.
    return desc_.global.seaLevelMeters + shoulder +
        headroom * (1.0 - 1.0 / (1.0 + x + x * x));
}

f64 AnalyticTerrainSource::MountainShape(
    const math::Double3& direction,
    const f64 footprintMeters) const noexcept
{
    if (desc_.mountains.octaves == 0 || desc_.mountains.reliefMeters == 0.0)
    {
        return 0.0;
    }
    const f64 footprint = footprintMeters * warpFootprintScale_ * 2.0;
    // Unresolved positive ridge noise retains an approximate mean, unlike
    // zero-mean fBm. Dropping it outright would make distant ranges disappear.
    if (detail::DetailWeight(mountainBands_[0].wavelengthMeters, footprint) <= 0.0)
    {
        return ShapeRidge(kMeanRidgeSignal);
    }
    math::Double3 warped = direction;
    if (desc_.mountains.warpAmplitudeMeters > 0.0)
    {
        const f64 warpWeight = detail::DetailWeight(
            desc_.mountains.warpWavelengthMeters, footprintMeters);
        warped = warped + detail::VectorNoise3D(
            direction * warpFrequency_, desc_.seed ^ 0x8CB92BA72F3D8DD7ULL) *
            (desc_.mountains.warpAmplitudeMeters * warpWeight / planet_.radiusMeters);
    }

    f64 sum = 0.0;
    f64 feedback = 1.0;
    for (u32 i = 0; i < desc_.mountains.octaves; ++i)
    {
        const auto& band = mountainBands_[i];
        const f64 weight = detail::DetailWeight(band.wavelengthMeters, footprint);
        if (weight <= 0.0)
        {
            // Geometric tail, with no noise calls for the remaining octaves.
            const u32 remaining = desc_.mountains.octaves - i;
            const f64 tail = band.amplitude * 2.0 * (1.0 - std::ldexp(1.0, -static_cast<int>(remaining)));
            sum += kMeanRidgeSignal * feedback * tail;
            break;
        }
        const f64 noise = detail::ValueNoise3D(warped * band.frequency, band.seed);
        const f64 ridge = 1.0 - std::abs(noise);
        const f64 signal = ridge * ridge;
        sum += detail::Lerp(kMeanRidgeSignal, signal, weight) * feedback * band.amplitude;
        // Fine ridges grow from the preceding ridge, leaving valleys smoother.
        feedback *= detail::Lerp(1.0, std::clamp(signal * 2.0, 0.0, 1.0), weight);
        // Orthonormal rotation between octaves breaks persistent lattice axes.
        warped = {
            0.36 * warped.x + 0.48 * warped.y - 0.80 * warped.z,
           -0.80 * warped.x + 0.60 * warped.y,
            0.48 * warped.x + 0.64 * warped.y + 0.60 * warped.z
        };
    }
    return ShapeRidge(sum / mountainNormalization_);
}

TerrainSample AnalyticTerrainSource::Sample(
    const TerrainQuery& query) const noexcept
{
    const math::Double3 direction = math::Normalize(query.unitDirection);
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
        !std::isfinite(direction.z) || math::LengthSquared(direction) <= 0.0 ||
        !std::isfinite(query.footprintMeters))
    {
        return {};
    }
    const GlobalTerrainFieldSample global =
        globalFields_.SampleNormalized({direction, query.footprintMeters}, false);
    const f64 macroWeight = detail::DetailWeight(desc_.macroWavelengthMeters, query.footprintMeters);
    const f64 macroNoise = macroWeight > 0.0 ? detail::SampleBand(
        direction, planet_.radiusMeters, desc_.macroWavelengthMeters,
        desc_.seed ^ 0x632BE59BD9B4E019ULL) : 0.0;
    f64 elevation = LimitElevation(global.coarseElevationMeters +
        macroNoise * desc_.macroAmplitudeMeters * macroWeight);

    // Reuse continental and regional fields as a range selector. Smooth gates
    // preserve coastlines and make the expensive mountain branch absent at sea.
    const f64 coastMask = detail::Smooth((elevation - desc_.global.seaLevelMeters) / 700.0);
    const f64 rangeMask = detail::Lerp(0.5, detail::Smooth((macroNoise + 0.35) / 0.75), macroWeight);
    const f64 mountainMask = desc_.mountains.reliefMeters > 0.0 && desc_.mountains.octaves > 0
        ? global.landMask * coastMask * rangeMask : 0.0;
    const f64 ceiling = desc_.global.seaLevelMeters + desc_.maximumElevationAboveSeaLevelMeters;
    if (mountainMask > 0.0 && desc_.mountains.reliefMeters > 0.0)
    {
        const f64 availableRelief = std::min(desc_.mountains.reliefMeters, ceiling - elevation);
        elevation += mountainMask * availableRelief *
            MountainShape(direction, query.footprintMeters);
    }
    const f64 coarseElevation = elevation;
    // The full signed fBm sum is bounded by twice its initial amplitude.
    // Reserve that headroom before adding detail instead of flattening peaks
    // with a nonlinear cap after their ridges have already been generated.
    const f64 detailGain = std::min(1.0, (ceiling - elevation) /
        std::max(desc_.detailAmplitudeMeters * 2.0, 1.0));
    // Ridges already supply meso-scale relief inside ranges. Crossfade that
    // overlapping fBm away, keeping its fine surface texture and the complete
    // hill spectrum outside ranges. Exact zero skips those redundant bands.
    const f64 hillDetailWeight = 1.0 - detail::Smooth(mountainMask / 0.65);
    for (u32 i = 0; i < desc_.detailOctaves && desc_.detailAmplitudeMeters > 0.0; ++i)
    {
        const auto& band = detailBands_[i];
        const f64 weight = detail::DetailWeight(band.wavelengthMeters, query.footprintMeters);
        if (weight <= 0.0) break;
        const f64 landformWeight = band.wavelengthMeters > desc_.mountains.wavelengthMeters / 32.0
            ? hillDetailWeight : 1.0;
        if (landformWeight <= 0.0) continue;
        elevation += detail::ValueNoise3D(direction * band.frequency, band.seed) *
            band.amplitude * weight * detailGain * landformWeight;
    }
    TerrainClimate climate = global.climate;
    // Global climate used only the broad elevation. Apply the remaining lapse
    // once so high mountains are actually cold and classify consistently.
    climate.temperatureC -= static_cast<f32>(
        (std::max(elevation - desc_.global.seaLevelMeters, 0.0) -
         std::max(global.coarseElevationMeters - desc_.global.seaLevelMeters, 0.0)) *
        desc_.global.lapseRateCPerKilometer / 1'000.0);
    return {
        .elevationMeters = elevation,
        .coarseElevationMeters = coarseElevation,
        .climate = climate,
        .biomes = ClassifyBiomeWeights(climate, elevation, desc_.global.seaLevelMeters),
        .standingWaterDepthMeters = std::max(desc_.global.seaLevelMeters - elevation, 0.0)
    };
}

u64 AnalyticTerrainSource::Revision() const noexcept
{
    return revision_;
}
} // namespace orbit::terrain
