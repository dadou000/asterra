#include <orbit/terrain_relief/BaseReliefField.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_relief
{
namespace
{
struct NoiseValueDerivative
{
    f64 value{0.0};
    math::Double3 derivative{};
};

[[nodiscard]] u64 Mix64(u64 value) noexcept
{
    value += 0x9E3779B97F4A7C15ULL;
    value =
        (value ^ (value >> 30U)) *
        0xBF58476D1CE4E5B9ULL;
    value =
        (value ^ (value >> 27U)) *
        0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] u64 HashLattice(
    const i64 x,
    const i64 y,
    const i64 z,
    const u64 seed) noexcept
{
    u64 hash = seed;
    hash ^= Mix64(static_cast<u64>(x) + 0x632BE59BD9B4E019ULL);
    hash ^= Mix64(static_cast<u64>(y) + 0x8CB92BA72F3D8DD7ULL);
    hash ^= Mix64(static_cast<u64>(z) + 0x58F38DED8C5A935FULL);
    return Mix64(hash);
}

[[nodiscard]] f64 HashValue(
    const i64 x,
    const i64 y,
    const i64 z,
    const u64 seed) noexcept
{
    constexpr f64 inverse53 =
        1.0 / static_cast<f64>(1ULL << 53U);

    return
        static_cast<f64>(
            HashLattice(x, y, z, seed) >> 11U) *
            inverse53 * 2.0 -
        1.0;
}

[[nodiscard]] f64 Quintic(const f64 t) noexcept
{
    const f64 x = std::clamp(t, 0.0, 1.0);
    return x * x * x *
        (x * (x * 6.0 - 15.0) + 10.0);
}

[[nodiscard]] f64 QuinticDerivative(const f64 t) noexcept
{
    const f64 x = std::clamp(t, 0.0, 1.0);
    const f64 oneMinus = 1.0 - x;
    return 30.0 * x * x * oneMinus * oneMinus;
}

[[nodiscard]] NoiseValueDerivative LerpX(
    const NoiseValueDerivative& a,
    const NoiseValueDerivative& b,
    const f64 t,
    const f64 dt) noexcept
{
    NoiseValueDerivative result;
    result.value = a.value + (b.value - a.value) * t;
    result.derivative =
        a.derivative +
        (b.derivative - a.derivative) * t;
    result.derivative.x +=
        (b.value - a.value) * dt;
    return result;
}

[[nodiscard]] NoiseValueDerivative LerpY(
    const NoiseValueDerivative& a,
    const NoiseValueDerivative& b,
    const f64 t,
    const f64 dt) noexcept
{
    NoiseValueDerivative result;
    result.value = a.value + (b.value - a.value) * t;
    result.derivative =
        a.derivative +
        (b.derivative - a.derivative) * t;
    result.derivative.y +=
        (b.value - a.value) * dt;
    return result;
}

[[nodiscard]] NoiseValueDerivative LerpZ(
    const NoiseValueDerivative& a,
    const NoiseValueDerivative& b,
    const f64 t,
    const f64 dt) noexcept
{
    NoiseValueDerivative result;
    result.value = a.value + (b.value - a.value) * t;
    result.derivative =
        a.derivative +
        (b.derivative - a.derivative) * t;
    result.derivative.z +=
        (b.value - a.value) * dt;
    return result;
}

[[nodiscard]] NoiseValueDerivative ValueNoise3DWithDerivative(
    const math::Double3& position,
    const u64 seed) noexcept
{
    const i64 x0 = static_cast<i64>(std::floor(position.x));
    const i64 y0 = static_cast<i64>(std::floor(position.y));
    const i64 z0 = static_cast<i64>(std::floor(position.z));

    const f64 tx = position.x - static_cast<f64>(x0);
    const f64 ty = position.y - static_cast<f64>(y0);
    const f64 tz = position.z - static_cast<f64>(z0);

    const f64 wx = Quintic(tx);
    const f64 wy = Quintic(ty);
    const f64 wz = Quintic(tz);

    const f64 dx = QuinticDerivative(tx);
    const f64 dy = QuinticDerivative(ty);
    const f64 dz = QuinticDerivative(tz);

    std::array<NoiseValueDerivative, 8> corner{};
    for (i64 z = 0; z <= 1; ++z)
    {
        for (i64 y = 0; y <= 1; ++y)
        {
            for (i64 x = 0; x <= 1; ++x)
            {
                const std::size_t index =
                    static_cast<std::size_t>(
                        x + y * 2 + z * 4);
                corner[index].value =
                    HashValue(
                        x0 + x,
                        y0 + y,
                        z0 + z,
                        seed);
            }
        }
    }

    const auto x00 = LerpX(corner[0], corner[1], wx, dx);
    const auto x10 = LerpX(corner[2], corner[3], wx, dx);
    const auto x01 = LerpX(corner[4], corner[5], wx, dx);
    const auto x11 = LerpX(corner[6], corner[7], wx, dx);

    const auto y0v = LerpY(x00, x10, wy, dy);
    const auto y1v = LerpY(x01, x11, wy, dy);

    return LerpZ(y0v, y1v, wz, dz);
}

[[nodiscard]] f64 SmoothUnit(const f64 value) noexcept
{
    return Quintic(std::clamp(value, 0.0, 1.0));
}

struct HeightDerivativeAccum
{
    f64 height{0.0};
    f64 east{0.0};
    f64 north{0.0};
    f64 rawSignal{0.0};
};

[[nodiscard]] HeightDerivativeAccum SignedBand(
    const math::Double3& direction,
    const world::SurfaceFrame& frame,
    const f64 planetRadiusMeters,
    const f64 wavelengthMeters,
    const f64 amplitudeMeters,
    const u64 seed,
    const f64 weight) noexcept
{
    if (weight <= 0.0 ||
        wavelengthMeters <= 0.0 ||
        amplitudeMeters == 0.0)
    {
        return {};
    }

    const f64 frequency =
        planetRadiusMeters / wavelengthMeters;

    const NoiseValueDerivative noise =
        ValueNoise3DWithDerivative(
            direction * frequency,
            seed);

    const f64 derivativeScale =
        amplitudeMeters * weight /
        wavelengthMeters;

    return {
        .height = noise.value * amplitudeMeters * weight,
        .east =
            math::Dot(noise.derivative, frame.east) *
            derivativeScale,
        .north =
            math::Dot(noise.derivative, frame.north) *
            derivativeScale,
        .rawSignal = noise.value
    };
}

[[nodiscard]] HeightDerivativeAccum RidgedBand(
    const math::Double3& direction,
    const world::SurfaceFrame& frame,
    const f64 planetRadiusMeters,
    const f64 wavelengthMeters,
    const f64 amplitudeMeters,
    const u64 seed,
    const f64 weight,
    const f64 sign) noexcept
{
    if (weight <= 0.0 ||
        wavelengthMeters <= 0.0 ||
        amplitudeMeters == 0.0)
    {
        return {};
    }

    const f64 frequency =
        planetRadiusMeters / wavelengthMeters;

    const NoiseValueDerivative noise =
        ValueNoise3DWithDerivative(
            direction * frequency,
            seed);

    const f64 absolute = std::abs(noise.value);
    const f64 ridgeBase = std::max(0.0, 1.0 - absolute);
    const f64 ridge = ridgeBase * ridgeBase * ridgeBase;

    f64 dRidgeDNoise = 0.0;
    if (noise.value > 0.0)
    {
        dRidgeDNoise =
            -3.0 * ridgeBase * ridgeBase;
    }
    else if (noise.value < 0.0)
    {
        dRidgeDNoise =
            3.0 * ridgeBase * ridgeBase;
    }

    const f64 derivativeScale =
        sign * amplitudeMeters * weight *
        dRidgeDNoise / wavelengthMeters;

    return {
        .height =
            sign * ridge * amplitudeMeters * weight,
        .east =
            math::Dot(noise.derivative, frame.east) *
            derivativeScale,
        .north =
            math::Dot(noise.derivative, frame.north) *
            derivativeScale,
        .rawSignal = ridge
    };
}

[[nodiscard]] bool FinitePositive(
    const f64 value) noexcept
{
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] bool FiniteNonNegative(
    const f64 value) noexcept
{
    return std::isfinite(value) && value >= 0.0;
}
} // namespace

bool BaseReliefDesc::IsValid() const noexcept
{
    return
        FiniteNonNegative(continentalAmplitudeMeters) &&
        FinitePositive(continentalWavelengthMeters) &&
        std::isfinite(continentalBiasMeters) &&
        FiniteNonNegative(ridgeAmplitudeMeters) &&
        FinitePositive(ridgeWavelengthMeters) &&
        ridgeOctaves <= kMaxReliefOctaves &&
        FiniteNonNegative(valleyAmplitudeMeters) &&
        FinitePositive(valleyWavelengthMeters) &&
        valleyOctaves <= kMaxReliefOctaves &&
        std::isfinite(lacunarity) &&
        lacunarity > 1.0 &&
        std::isfinite(persistence) &&
        persistence > 0.0 &&
        persistence < 1.0;
}

f64 ReliefFrequencyWeight(
    const f64 wavelengthMeters,
    const f64 footprintMeters) noexcept
{
    if (!std::isfinite(wavelengthMeters) ||
        wavelengthMeters <= 0.0)
    {
        return 0.0;
    }

    if (!std::isfinite(footprintMeters) ||
        footprintMeters <= 0.0)
    {
        return 1.0;
    }

    const f64 lower = footprintMeters * 2.0;
    const f64 upper = footprintMeters * 4.0;

    if (wavelengthMeters <= lower)
    {
        return 0.0;
    }

    if (wavelengthMeters >= upper)
    {
        return 1.0;
    }

    return SmoothUnit(
        (wavelengthMeters - lower) /
        (upper - lower));
}

BaseReliefField::BaseReliefField(
    const world::PlanetDefinition planet,
    const terrain_macro_geology::MacroGeologyField* macroGeology,
    BaseReliefDesc desc)
    : planet_(planet),
      macroGeology_(macroGeology),
      desc_(std::move(desc)),
      resolvedSeed_(
          desc_.seed != 0
              ? desc_.seed
              : Mix64(
                    planet.generationSeed ^
                    0x4D303652454C4946ULL))
{
    if (!planet_.id.IsValid() ||
        !FinitePositive(planet_.radiusMeters))
    {
        throw std::invalid_argument(
            "M06 BaseReliefField requires a valid planet.");
    }

    if (!desc_.IsValid())
    {
        throw std::invalid_argument(
            "M06 BaseReliefDesc contains invalid values.");
    }

    auto prepare =
        [&](auto& bands,
            const u32 count,
            f64 wavelength,
            f64 amplitude,
            const u64 salt)
        {
            for (u32 index = 0; index < count; ++index)
            {
                bands[index] = {
                    .wavelengthMeters = wavelength,
                    .amplitudeMeters = amplitude,
                    .seed =
                        Mix64(
                            resolvedSeed_ ^
                            salt ^
                            static_cast<u64>(index + 1U) *
                                0x9E3779B97F4A7C15ULL)
                };

                wavelength /= desc_.lacunarity;
                amplitude *= desc_.persistence;
            }
        };

    prepare(
        ridgeBands_,
        desc_.ridgeOctaves,
        desc_.ridgeWavelengthMeters,
        desc_.ridgeAmplitudeMeters,
        0x52494447454D3036ULL);

    prepare(
        valleyBands_,
        desc_.valleyOctaves,
        desc_.valleyWavelengthMeters,
        desc_.valleyAmplitudeMeters,
        0x56414C4C45594D36ULL);
}

BaseReliefSample BaseReliefField::Sample(
    const terrain::PlanetSurfacePosition& position,
    const terrain::TerrainSampleFootprint& footprint) const
{
    const terrain::PlanetSurfacePosition canonical =
        terrain::CanonicalizeSurfacePosition(position);

    if (!canonical.IsValid() ||
        canonical.planet != planet_.id)
    {
        throw std::invalid_argument(
            "M06 relief position is invalid or belongs to another planet.");
    }

    if (!footprint.IsValid())
    {
        throw std::invalid_argument(
            "M06 relief footprint must be finite and positive.");
    }

    const world::SurfaceFrame frame =
        terrain::SurfaceTangentFrame(canonical);

    const u64 continentalSeed =
        Mix64(resolvedSeed_ ^ 0x434F4E544D303036ULL);

    const f64 continentalWeight =
        ReliefFrequencyWeight(
            desc_.continentalWavelengthMeters,
            footprint.diameterMeters);

    const HeightDerivativeAccum continental =
        SignedBand(
            canonical.unitDirection,
            frame,
            planet_.radiusMeters,
            desc_.continentalWavelengthMeters,
            desc_.continentalAmplitudeMeters,
            continentalSeed,
            continentalWeight);

    BaseReliefSample sample{};
    sample.baseContinentalMeters =
        desc_.continentalBiasMeters +
        continental.height;
    sample.derivative.east = continental.east;
    sample.derivative.north = continental.north;

    f64 ridgeWeightSum = 0.0;
    f64 valleyWeightSum = 0.0;

    for (u32 index = 0;
         index < desc_.ridgeOctaves;
         ++index)
    {
        const Band& band = ridgeBands_[index];
        const f64 weight =
            ReliefFrequencyWeight(
                band.wavelengthMeters,
                footprint.diameterMeters);

        if (weight <= 0.0)
        {
            continue;
        }

        const HeightDerivativeAccum contribution =
            RidgedBand(
                canonical.unitDirection,
                frame,
                planet_.radiusMeters,
                band.wavelengthMeters,
                band.amplitudeMeters,
                band.seed,
                weight,
                1.0);

        sample.ridgeDetailMeters +=
            contribution.height;
        sample.derivative.east +=
            contribution.east;
        sample.derivative.north +=
            contribution.north;
        sample.ridgeSignal +=
            contribution.rawSignal * weight;
        ridgeWeightSum += weight;
        ++sample.activeDetailBands;
    }

    for (u32 index = 0;
         index < desc_.valleyOctaves;
         ++index)
    {
        const Band& band = valleyBands_[index];
        const f64 weight =
            ReliefFrequencyWeight(
                band.wavelengthMeters,
                footprint.diameterMeters);

        if (weight <= 0.0)
        {
            continue;
        }

        const HeightDerivativeAccum contribution =
            RidgedBand(
                canonical.unitDirection,
                frame,
                planet_.radiusMeters,
                band.wavelengthMeters,
                band.amplitudeMeters,
                band.seed,
                weight,
                -1.0);

        sample.valleyDetailMeters +=
            contribution.height;
        sample.derivative.east +=
            contribution.east;
        sample.derivative.north +=
            contribution.north;
        sample.valleySignal +=
            contribution.rawSignal * weight;
        valleyWeightSum += weight;
        ++sample.activeDetailBands;
    }

    if (ridgeWeightSum > 0.0)
    {
        sample.ridgeSignal =
            std::clamp(
                sample.ridgeSignal / ridgeWeightSum,
                0.0,
                1.0);
    }

    if (valleyWeightSum > 0.0)
    {
        sample.valleySignal =
            std::clamp(
                sample.valleySignal / valleyWeightSum,
                0.0,
                1.0);
    }

    sample.geologicalDetailSignal =
        std::clamp(
            sample.ridgeSignal - sample.valleySignal,
            -1.0,
            1.0);

    sample.heightMeters =
        sample.baseContinentalMeters +
        sample.ridgeDetailMeters +
        sample.valleyDetailMeters;

    if (macroGeology_ != nullptr)
    {
        const auto macro =
            macroGeology_->Sample(canonical);

        sample.macroUpliftMeters =
            macro.upliftMeters;
        sample.drainageGuidance =
            macro.drainageGuidance;
        sample.protection =
            macro.protection;
    }

    return sample;
}

const BaseReliefDesc&
BaseReliefField::Description() const noexcept
{
    return desc_;
}
} // namespace orbit::terrain_relief
