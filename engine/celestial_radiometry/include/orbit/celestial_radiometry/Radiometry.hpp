#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

namespace orbit::celestial_radiometry
{
// CODATA/NIST-derived Stefan-Boltzmann constant in SI.
inline constexpr f64 kStefanBoltzmannWattsPerSquareMeterKelvin4 =
    5.670374419e-8;

struct BlackbodyEmitter
{
    f64 radiusMeters{1.0};
    f64 effectiveTemperatureKelvin{5772.0};
    f64 emissivity{1.0};
    f64 explicitLuminosityWatts{0.0};
    bool deriveLuminosity{true};
};

struct RadiativeState
{
    f64 luminosityWatts{0.0};
    f64 surfaceExitanceWattsPerSquareMeter{0.0};
    f64 surfaceRadianceWattsPerSquareMeterSteradian{0.0};
    f64 effectiveTemperatureKelvin{0.0};
    f64 emissivity{0.0};
};

[[nodiscard]] RadiativeState Resolve(
    const BlackbodyEmitter& emitter);

[[nodiscard]] f64 IrradianceWattsPerSquareMeter(
    f64 luminosityWatts,
    f64 distanceMeters);

[[nodiscard]] f64 CentralPixelSolidAngleSteradians(
    f64 verticalFieldOfViewRadians,
    u32 viewportHeightPixels);

[[nodiscard]] f64 ResolvedPixelIrradianceWattsPerSquareMeter(
    f64 radianceWattsPerSquareMeterSteradian,
    f64 verticalFieldOfViewRadians,
    u32 viewportHeightPixels);

struct SceneEncodingSettings
{
    // Fixed conversion from physical irradiance to Orbit scene-linear units.
    // This is a unit/calibration mapping, not view exposure or eye adaptation.
    f64 referenceIrradianceWattsPerSquareMeter{1361.0};
    f64 referenceSceneValue{0.18};
};

[[nodiscard]] f64 EncodeIrradianceSceneLinear(
    f64 irradianceWattsPerSquareMeter,
    const SceneEncodingSettings& settings = {});

struct ExposureSettings
{
    // Explicit calibration reference. An irradiance equal to this value is
    // mapped to middleGray before tone mapping at compensationStops == 0.
    f64 referenceIrradianceWattsPerSquareMeter{1361.0};
    f64 middleGray{0.18};
    f64 compensationStops{0.0};
};

struct ExposureState
{
    f64 scalePerWattPerSquareMeter{0.0};
    f64 referenceIrradianceWattsPerSquareMeter{0.0};
    f64 middleGray{0.18};
    f64 compensationStops{0.0};
};

[[nodiscard]] ExposureState ResolveExposure(
    const ExposureSettings& settings);

[[nodiscard]] f64 ExposeIrradiance(
    f64 irradianceWattsPerSquareMeter,
    const ExposureState& exposure) noexcept;

[[nodiscard]] f64 ToneMapReinhard(
    f64 sceneLinear) noexcept;

[[nodiscard]] math::Double3 ToneMapReinhard(
    const math::Double3 sceneLinear) noexcept;
} // namespace orbit::celestial_radiometry
