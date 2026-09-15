#include <orbit/terrain/AnalyticTerrainSource.hpp>

#include "ProceduralNoise.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
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

// Sign of the prevailing zonal wind at a given latitude, Earth's 3-cell
// pattern: trade winds (0-30, blow west), westerlies (30-60, blow east),
// polar easterlies (60-90, blow west again). Smoothly blended across each
// breakpoint (width `transitionDegrees`) rather than switching abruptly,
// so the rain-shadow probe direction never jumps discontinuously.
[[nodiscard]] f64 ZonalWindSign(
    const f64 latitudeDeg,
    const f64 transitionDegrees) noexcept
{
    const f64 absLatitude = std::abs(latitudeDeg);
    const f64 halfWidth = std::max(transitionDegrees, 0.1) * 0.5;

    const f64 tradeToWesterly = detail::Smooth(
        (absLatitude - (30.0 - halfWidth)) / (2.0 * halfWidth));
    const f64 westerlyToPolar = detail::Smooth(
        (absLatitude - (60.0 - halfWidth)) / (2.0 * halfWidth));

    const f64 throughWesterlies = detail::Lerp(-1.0, 1.0, tradeToWesterly);
    return detail::Lerp(throughWesterlies, -1.0, westerlyToPolar);
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
             desc.global.continentalAmplitudeMeters, desc.global.mountainAmplitudeMeters,
             desc.global.tectonic.plateIrregularity, desc.global.tectonic.boundaryWidthDot,
             desc.global.tectonic.minPlateAngularSpeed, desc.global.tectonic.maxPlateAngularSpeed,
             desc.global.tectonic.convergenceReferenceSpeed, desc.global.tectonic.oceanicConvergenceScale,
             desc.global.tectonic.convergenceUpliftMeters, desc.global.tectonic.hotspotBaseReliefMeters,
             desc.global.tectonic.hotspotAgeDecay, desc.global.tectonic.hotspotChainSpacingMeters,
             desc.global.tectonic.hotspotCoreRadiusMeters, desc.global.tectonic.hotspotRadiusGrowthPerAge,
             desc.global.tectonic.rainShadowStrength, desc.global.tectonic.rainShadowStepMeters,
             desc.global.tectonic.rainShadowStepGrowth, desc.global.tectonic.rainShadowRangeMeters,
             desc.global.tectonic.windBandTransitionDegrees})
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
    mix(desc.global.tectonic.seed);
    mix(desc.global.tectonic.plateCount);
    mix(desc.global.tectonic.hotspotCount);
    mix(desc.global.tectonic.hotspotAgeSteps);
    mix(desc.global.tectonic.rainShadowSteps);
    for (const f64 value : {planet.radiusMeters, desc.macroAmplitudeMeters,
             desc.macroWavelengthMeters, desc.detailAmplitudeMeters, desc.detailWavelengthMeters,
             desc.mountains.reliefMeters, desc.mountains.wavelengthMeters,
             desc.mountains.warpWavelengthMeters, desc.mountains.warpAmplitudeMeters,
             desc.maximumElevationAboveSeaLevelMeters, desc.global.seaLevelMeters,
             desc.global.continentalAmplitudeMeters, desc.global.continentalWavelengthMeters,
             desc.global.continentalBiasMeters, desc.global.mountainAmplitudeMeters,
             desc.global.mountainWavelengthMeters, desc.global.climateWavelengthMeters,
             desc.global.equatorTemperatureC, desc.global.poleTemperatureC,
             desc.global.temperatureVariationC, desc.global.lapseRateCPerKilometer,
             desc.global.tectonic.plateIrregularity, desc.global.tectonic.continentalPlateFraction,
             desc.global.tectonic.continentalPlateBiasMeters, desc.global.tectonic.oceanicPlateBiasMeters,
             desc.global.tectonic.tectonicContinentInfluence, desc.global.tectonic.boundaryWidthDot,
             desc.global.tectonic.minPlateAngularSpeed, desc.global.tectonic.maxPlateAngularSpeed,
             desc.global.tectonic.convergenceReferenceSpeed, desc.global.tectonic.oceanicConvergenceScale,
             desc.global.tectonic.convergenceUpliftMeters, desc.global.tectonic.hotspotBaseReliefMeters,
             desc.global.tectonic.hotspotAgeDecay, desc.global.tectonic.hotspotChainSpacingMeters,
             desc.global.tectonic.hotspotCoreRadiusMeters, desc.global.tectonic.hotspotRadiusGrowthPerAge,
             desc.global.tectonic.rainShadowStrength, desc.global.tectonic.rainShadowStepMeters,
             desc.global.tectonic.rainShadowStepGrowth, desc.global.tectonic.rainShadowThresholdMeters,
             desc.global.tectonic.rainShadowRangeMeters, desc.global.tectonic.windBandTransitionDegrees})
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

    // Reuse continental fields and coherent plate-boundary convergence as a
    // range selector. Smooth gates preserve coastlines and make the
    // expensive mountain branch absent at sea. convergenceMask is a
    // function of direction only (see TectonicField::Sample), so it can't
    // introduce a footprint-dependent discontinuity here.
    const f64 coastMask = detail::Smooth((elevation - desc_.global.seaLevelMeters) / 700.0);
    const f64 rangeMask = global.convergenceMask;
    const f64 mountainMask = desc_.mountains.reliefMeters > 0.0 && desc_.mountains.octaves > 0
        ? global.landMask * coastMask * rangeMask : 0.0;
    const f64 ceiling = desc_.global.seaLevelMeters + desc_.maximumElevationAboveSeaLevelMeters;
    if (mountainMask > 0.0 && desc_.mountains.reliefMeters > 0.0)
    {
        const f64 availableRelief = std::min(desc_.mountains.reliefMeters, ceiling - elevation);
        elevation += mountainMask * availableRelief *
            MountainShape(direction, query.footprintMeters);
    }
    if (global.hotspotElevationMeters > 0.0)
    {
        const f64 headroom = std::max(0.0, ceiling - elevation);
        elevation += std::min(global.hotspotElevationMeters, headroom);
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

    // Rain shadow: attenuate precipitation/humidity when a taller range
    // sits between this point and the prevailing wind's source, so
    // biomes actually respond to the coherent mountain chains above
    // (dry leeward deserts, unattenuated wet windward coasts -- the
    // latter already falls out of the existing continentality term).
    // Uses only the cheap coarse plate/continental estimate upwind, never
    // the full detailed Sample() recursively.
    if (desc_.global.tectonic.rainShadowStrength > 0.0 &&
        desc_.global.tectonic.rainShadowSteps > 0)
    {
        const f64 latitudeDeg = std::asin(std::clamp(direction.y, -1.0, 1.0)) *
            (180.0 / std::numbers::pi);
        const f64 windSign = ZonalWindSign(
            latitudeDeg, desc_.global.tectonic.windBandTransitionDegrees);
        const world::SurfaceFrame frame = world::MakeSurfaceFrame(direction);

        f64 blockingHeightMeters = -1.0e30;
        f64 stepDistanceMeters = desc_.global.tectonic.rainShadowStepMeters;
        for (u32 step = 0; step < desc_.global.tectonic.rainShadowSteps; ++step)
        {
            // Upwind is where the wind blows FROM: a positive windSign
            // means wind blows eastward, so upwind is west (negative
            // east offset).
            const math::Double3 upwindDirection = world::DirectionAtSurfaceOffset(
                planet_, frame, math::Double2{-windSign * stepDistanceMeters, 0.0});
            blockingHeightMeters = std::max(
                blockingHeightMeters,
                globalFields_.PlateElevationEstimateMeters(upwindDirection));
            stepDistanceMeters *= desc_.global.tectonic.rainShadowStepGrowth;
        }

        const f64 shadow = detail::Smooth(
            (blockingHeightMeters - coarseElevation -
             desc_.global.tectonic.rainShadowThresholdMeters) /
            std::max(desc_.global.tectonic.rainShadowRangeMeters, 1.0));

        climate.precipitation = static_cast<f32>(
            climate.precipitation *
            (1.0 - desc_.global.tectonic.rainShadowStrength * shadow));
        climate.humidity = static_cast<f32>(
            climate.humidity *
            (1.0 - desc_.global.tectonic.rainShadowStrength * 0.7 * shadow));
    }

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
