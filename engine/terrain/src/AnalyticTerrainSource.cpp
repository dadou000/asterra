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

[[nodiscard]] f64 UnitFloat(const u64 value) noexcept
{
    constexpr f64 inverse53 = 1.0 / static_cast<f64>(1ULL << 53U);
    return static_cast<f64>(value >> 11U) * inverse53;
}

[[nodiscard]] f64 FeatureWeight(
    const f64 diameterMeters,
    const f64 footprintMeters) noexcept
{
    const f64 lower = footprintMeters * 2.0;
    const f64 upper = footprintMeters * 4.0;
    if (diameterMeters <= lower) return 0.0;
    if (diameterMeters >= upper) return 1.0;
    return detail::Smooth((diameterMeters - lower) /
        std::max(upper - lower, 1.0));
}

[[nodiscard]] f64 HashUnit24(const u64 hash) noexcept
{
    constexpr f64 inverse24 = 1.0 / 16'777'216.0;
    return static_cast<f64>(hash >> 40U) * inverse24;
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
    if (desc.craters.count > 4096U ||
        !std::isfinite(desc.craters.minimumRadiusMeters) ||
        !std::isfinite(desc.craters.maximumRadiusMeters) ||
        !std::isfinite(desc.craters.cumulativeExponent) ||
        !std::isfinite(desc.craters.complexTransitionRadiusMeters) ||
        !std::isfinite(desc.craters.maximumEjectaExtentRadii) ||
        !std::isfinite(desc.craters.localBaseSpacingMeters) ||
        !std::isfinite(desc.craters.localDensity) ||
        desc.craters.minimumRadiusMeters <= 0.0 ||
        desc.craters.maximumRadiusMeters < desc.craters.minimumRadiusMeters ||
        desc.craters.cumulativeExponent <= 0.0 ||
        desc.craters.complexTransitionRadiusMeters <= 0.0 ||
        desc.craters.maximumEjectaExtentRadii < 1.0 ||
        desc.craters.localLevels > 6U ||
        desc.craters.localBaseSpacingMeters <= 0.0 ||
        desc.craters.localDensity < 0.0 ||
        desc.craters.localDensity > 1.0)
    {
        throw std::invalid_argument("Orbit procedural crater recipe is invalid.");
    }

    if (desc.craters.enabled)
    {
        craters_.reserve(desc.craters.count);
        const u64 craterSeed = detail::Mix64(
            desc.seed ^ 0x4352415445525031ULL);
        const f64 minimumPower = std::pow(
            desc.craters.minimumRadiusMeters,
            -desc.craters.cumulativeExponent);
        const f64 maximumPower = std::pow(
            desc.craters.maximumRadiusMeters,
            -desc.craters.cumulativeExponent);

        for (u32 index = 0; index < desc.craters.count; ++index)
        {
            const u64 ordinal = static_cast<u64>(index) + 1ULL;
            const f64 u = UnitFloat(detail::Mix64(
                craterSeed ^ ordinal * 0x9E3779B97F4A7C15ULL));
            const f64 v = UnitFloat(detail::Mix64(
                craterSeed ^ ordinal * 0xBF58476D1CE4E5B9ULL));
            const f64 radiusChoice = UnitFloat(detail::Mix64(
                craterSeed ^ ordinal * 0x94D049BB133111EBULL));
            const f64 z = 1.0 - 2.0 * u;
            const f64 radial = std::sqrt(std::max(0.0, 1.0 - z * z));
            const f64 azimuth = 2.0 * std::numbers::pi_v<f64> * v;
            const f64 radiusPower = minimumPower +
                (maximumPower - minimumPower) * radiusChoice;
            craters_.push_back({
                .centerDirection = {
                    radial * std::cos(azimuth), z,
                    radial * std::sin(azimuth)},
                .radiusMeters = std::pow(
                    std::max(radiusPower, maximumPower),
                    -1.0 / desc.craters.cumulativeExponent),
                .degradation = UnitFloat(detail::Mix64(
                    craterSeed ^ ordinal * 0xD6E8FEB86659FD93ULL)) * 0.58,
                .rimIrregularityPhase = 2.0 * std::numbers::pi_v<f64> *
                    UnitFloat(detail::Mix64(
                        craterSeed ^ ordinal * 0xA24BAED4963EE407ULL)),
                .boundingCosine = std::cos(std::min(
                    std::pow(
                        std::max(radiusPower, maximumPower),
                        -1.0 / desc.craters.cumulativeExponent) *
                        desc.craters.maximumEjectaExtentRadii /
                        planet.radiusMeters,
                    std::numbers::pi_v<f64>))
            });
        }
        std::sort(
            craters_.begin(), craters_.end(),
            [](const GpuProceduralCrater& a,
               const GpuProceduralCrater& b)
            {
                return a.radiusMeters > b.radiusMeters;
            });
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
    mix(desc.craters.enabled ? 1U : 0U);
    mix(desc.craters.count);
    mix(desc.craters.localLevels);
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
    for (const f64 value : {desc.craters.minimumRadiusMeters,
             desc.craters.maximumRadiusMeters, desc.craters.cumulativeExponent,
             desc.craters.complexTransitionRadiusMeters,
             desc.craters.maximumEjectaExtentRadii,
             desc.craters.localBaseSpacingMeters,
             desc.craters.localDensity})
    {
        mix(std::bit_cast<u64>(value));
    }
}

f64 AnalyticTerrainSource::LocalCraterHeightDelta(
    const math::Double3& direction,
    const f64 footprintMeters) const noexcept
{
    f64 result = 0.0;
    f64 spacingMeters = desc_.craters.localBaseSpacingMeters;
    const u64 baseSeed = desc_.seed ^ 0x4C4F43414C435231ULL;

    for (u32 level = 0; level < desc_.craters.localLevels; ++level)
    {
        // The largest diameter in this level is 0.52 * spacing. Because
        // levels only shrink, all finer levels can be skipped together.
        if (FeatureWeight(spacingMeters * 0.52, footprintMeters) <= 0.0)
        {
            break;
        }

        const f64 frequency = planet_.radiusMeters / spacingMeters;
        const math::Double3 lattice = direction * frequency;
        const i64 cellX = static_cast<i64>(std::floor(lattice.x));
        const i64 cellY = static_cast<i64>(std::floor(lattice.y));
        const i64 cellZ = static_cast<i64>(std::floor(lattice.z));
        const u64 levelSeed = detail::Mix64(
            baseSeed ^ (static_cast<u64>(level) + 1ULL) *
                0x9E3779B97F4A7C15ULL);

        for (i64 z = cellZ - 1; z <= cellZ + 1; ++z)
        for (i64 y = cellY - 1; y <= cellY + 1; ++y)
        for (i64 x = cellX - 1; x <= cellX + 1; ++x)
        {
            const u64 cellHash = detail::HashLattice(x, y, z, levelSeed);
            if (HashUnit24(detail::Mix64(
                    cellHash ^ 0x4143544956455031ULL)) >=
                desc_.craters.localDensity)
            {
                continue;
            }

            const f64 rx = HashUnit24(detail::Mix64(
                cellHash ^ 0x504F534954494F58ULL));
            const f64 ry = HashUnit24(detail::Mix64(
                cellHash ^ 0x504F534954494F59ULL));
            const f64 rz = HashUnit24(detail::Mix64(
                cellHash ^ 0x504F534954494F5AULL));
            const math::Double3 candidate = math::Normalize(math::Double3{
                (static_cast<f64>(x) + rx) / frequency,
                (static_cast<f64>(y) + ry) / frequency,
                (static_cast<f64>(z) + rz) / frequency});

            const f64 radiusChoice = HashUnit24(detail::Mix64(
                cellHash ^ 0x5241444955535031ULL));
            const f64 craterRadius = spacingMeters *
                (0.08 + 0.18 * radiusChoice * radiusChoice);
            const f64 spectralWeight = FeatureWeight(
                craterRadius * 2.0, footprintMeters);
            if (spectralWeight <= 0.0) continue;

            const f64 cosine = std::clamp(
                math::Dot(candidate, direction), -1.0, 1.0);
            const f64 boundingCosine = std::cos(std::min(
                craterRadius * 1.55 / planet_.radiusMeters,
                std::numbers::pi_v<f64>));
            if (cosine < boundingCosine) continue;

            const f64 craterDistance = std::acos(cosine) *
                planet_.radiusMeters / craterRadius;
            const f64 age = HashUnit24(detail::Mix64(
                cellHash ^ 0x4445475241444550ULL));
            const f64 preservation = (0.48 + 0.52 * age) * spectralWeight;
            f64 delta = 0.0;
            if (craterDistance < 1.0)
            {
                const f64 bowl = std::max(
                    0.0, 1.0 - craterDistance * craterDistance);
                delta -= craterRadius * 0.16 * bowl * bowl;
            }
            const f64 rimDistance = (craterDistance - 1.0) / 0.085;
            delta += craterRadius * 0.030 *
                std::exp(-0.5 * rimDistance * rimDistance);
            if (craterDistance >= 1.0 && craterDistance <= 1.55)
            {
                const f64 ejectaT = (craterDistance - 1.0) / 0.55;
                delta += craterRadius * 0.007 *
                    std::pow(craterDistance, -3.0) *
                    (1.0 - detail::Smooth(ejectaT));
            }
            result += delta * preservation;
        }
        spacingMeters *= 0.25;
    }
    return result;
}

f64 AnalyticTerrainSource::CraterHeightDelta(
    const math::Double3& direction,
    const f64 footprintMeters) const noexcept
{
    f64 result = 0.0;
    for (const GpuProceduralCrater& crater : craters_)
    {
        const f64 spectralWeight = FeatureWeight(
            crater.radiusMeters * 2.0, footprintMeters);
        if (spectralWeight <= 0.0) break;
        const f64 cosine = std::clamp(
            math::Dot(crater.centerDirection, direction), -1.0, 1.0);
        if (cosine < crater.boundingCosine) continue;

        const f64 x = std::acos(cosine) * planet_.radiusMeters /
            crater.radiusMeters;
        const f64 preservation = (1.0 - crater.degradation) * spectralWeight;
        const bool complex = crater.radiusMeters >=
            desc_.craters.complexTransitionRadiusMeters;
        f64 delta = 0.0;

        if (x < 1.0)
        {
            const f64 bowl = std::max(0.0, 1.0 - x * x);
            delta -= crater.radiusMeters * (complex ? 0.070 : 0.18) *
                bowl * bowl;
            if (complex && x < 0.28)
            {
                const f64 peak = 1.0 - x / 0.28;
                delta += crater.radiusMeters * 0.045 * peak * peak;
            }
            if (complex && x > 0.62)
            {
                const f64 terrace = (x - 0.62) / 0.38;
                delta += crater.radiusMeters * 0.010 *
                    std::sin(terrace * 3.0 * std::numbers::pi_v<f64>) *
                    (1.0 - terrace);
            }
        }

        // Direction-space modulation breaks perfect circular rims without
        // introducing cube-face coordinates or seams.
        const f64 rimNoise = std::sin(
            direction.x * 31.0 + direction.y * 43.0 +
            direction.z * 29.0 + crater.rimIrregularityPhase);
        const f64 rimCenter = 1.0 + rimNoise * 0.035;
        const f64 rimDistance = (x - rimCenter) / 0.10;
        delta += crater.radiusMeters * 0.035 *
            std::exp(-0.5 * rimDistance * rimDistance);

        if (x >= 1.0 && x <= desc_.craters.maximumEjectaExtentRadii)
        {
            const f64 extent = (x - 1.0) /
                std::max(desc_.craters.maximumEjectaExtentRadii - 1.0, 1.0e-9);
            delta += crater.radiusMeters * 0.010 *
                std::pow(std::max(x, 1.0), -3.0) *
                (1.0 - detail::Smooth(extent));
        }
        result += delta * preservation;
    }
    return result;
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
    const f64 craterDelta =
        CraterHeightDelta(direction, query.footprintMeters) +
        LocalCraterHeightDelta(direction, query.footprintMeters);
    elevation += craterDelta;
    const f64 coarseElevation = elevation;
    // The full signed fBm sum is bounded by twice its initial amplitude.
    // Reserve that headroom before adding detail instead of flattening peaks
    // with a nonlinear cap after their ridges have already been generated.
    const f64 detailGain = std::clamp((ceiling - elevation) /
        std::max(desc_.detailAmplitudeMeters * 2.0, 1.0), 0.0, 1.0);
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
