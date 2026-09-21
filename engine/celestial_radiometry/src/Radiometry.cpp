#include <orbit/celestial_radiometry/Radiometry.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace orbit::celestial_radiometry
{
namespace
{
void RequireFinitePositive(
    const f64 value,
    const char* message)
{
    if (!std::isfinite(value) ||
        value <= 0.0)
    {
        throw std::invalid_argument(message);
    }
}
} // namespace

RadiativeState Resolve(
    const BlackbodyEmitter& emitter)
{
    RequireFinitePositive(
        emitter.radiusMeters,
        "Emitter radius must be finite and positive.");
    RequireFinitePositive(
        emitter.effectiveTemperatureKelvin,
        "Emitter effective temperature must be finite and positive.");

    if (!std::isfinite(emitter.emissivity) ||
        emitter.emissivity < 0.0 ||
        emitter.emissivity > 1.0)
    {
        throw std::invalid_argument(
            "Emitter emissivity must be finite and in [0,1].");
    }

    if (!std::isfinite(
            emitter.explicitLuminosityWatts) ||
        emitter.explicitLuminosityWatts < 0.0)
    {
        throw std::invalid_argument(
            "Explicit luminosity must be finite and non-negative.");
    }

    const f64 t2 =
        emitter.effectiveTemperatureKelvin *
        emitter.effectiveTemperatureKelvin;
    const f64 t4 = t2 * t2;

    const f64 exitance =
        emitter.emissivity *
        kStefanBoltzmannWattsPerSquareMeterKelvin4 *
        t4;

    const f64 derivedLuminosity =
        4.0 *
        std::numbers::pi_v<f64> *
        emitter.radiusMeters *
        emitter.radiusMeters *
        exitance;

    const f64 luminosity =
        emitter.deriveLuminosity
            ? derivedLuminosity
            : emitter.explicitLuminosityWatts;

    if (!std::isfinite(luminosity) ||
        luminosity < 0.0)
    {
        throw std::overflow_error(
            "Emitter luminosity is not finite.");
    }

    return {
        .luminosityWatts = luminosity,
        .surfaceExitanceWattsPerSquareMeter =
            exitance,
        .surfaceRadianceWattsPerSquareMeterSteradian =
            exitance /
            std::numbers::pi_v<f64>,
        .effectiveTemperatureKelvin =
            emitter.effectiveTemperatureKelvin,
        .emissivity =
            emitter.emissivity
    };
}

f64 IrradianceWattsPerSquareMeter(
    const f64 luminosityWatts,
    const f64 distanceMeters)
{
    if (!std::isfinite(luminosityWatts) ||
        luminosityWatts < 0.0)
    {
        throw std::invalid_argument(
            "Luminosity must be finite and non-negative.");
    }

    RequireFinitePositive(
        distanceMeters,
        "Emitter distance must be finite and positive.");

    const f64 denominator =
        4.0 *
        std::numbers::pi_v<f64> *
        distanceMeters *
        distanceMeters;

    return luminosityWatts /
        denominator;
}

ExposureState ResolveExposure(
    const ExposureSettings& settings)
{
    RequireFinitePositive(
        settings.referenceIrradianceWattsPerSquareMeter,
        "Exposure reference irradiance must be finite and positive.");

    if (!std::isfinite(settings.middleGray) ||
        settings.middleGray <= 0.0 ||
        settings.middleGray >= 1.0)
    {
        throw std::invalid_argument(
            "Exposure middle gray must be finite and in (0,1).");
    }

    if (!std::isfinite(
            settings.compensationStops))
    {
        throw std::invalid_argument(
            "Exposure compensation must be finite.");
    }

    const f64 scale =
        settings.middleGray *
        std::exp2(
            settings.compensationStops) /
        settings.referenceIrradianceWattsPerSquareMeter;

    return {
        .scalePerWattPerSquareMeter = scale,
        .referenceIrradianceWattsPerSquareMeter =
            settings.referenceIrradianceWattsPerSquareMeter,
        .middleGray =
            settings.middleGray,
        .compensationStops =
            settings.compensationStops
    };
}

f64 ExposeIrradiance(
    const f64 irradianceWattsPerSquareMeter,
    const ExposureState& exposure) noexcept
{
    if (!std::isfinite(
            irradianceWattsPerSquareMeter) ||
        irradianceWattsPerSquareMeter <= 0.0 ||
        !std::isfinite(
            exposure.scalePerWattPerSquareMeter) ||
        exposure.scalePerWattPerSquareMeter <= 0.0)
    {
        return 0.0;
    }

    return irradianceWattsPerSquareMeter *
        exposure.scalePerWattPerSquareMeter;
}

f64 ToneMapReinhard(
    const f64 sceneLinear) noexcept
{
    if (!std::isfinite(sceneLinear) ||
        sceneLinear <= 0.0)
    {
        return 0.0;
    }

    return sceneLinear /
        (1.0 + sceneLinear);
}

math::Double3 ToneMapReinhard(
    const math::Double3 sceneLinear) noexcept
{
    return {
        ToneMapReinhard(sceneLinear.x),
        ToneMapReinhard(sceneLinear.y),
        ToneMapReinhard(sceneLinear.z)
    };
}
} // namespace orbit::celestial_radiometry
