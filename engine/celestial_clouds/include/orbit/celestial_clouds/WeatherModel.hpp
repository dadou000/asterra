#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

namespace orbit::celestial_clouds
{
// Simplified planetary weather: a deterministic, analytic function of position
// and time that organises cloud placement by the large-scale circulation instead
// of by uniform noise. It is deliberately not a fluid solver; it is the cheap,
// hot-reloadable "weather map" authority (coverage, type, precipitation) that
// the volumetric renderer then fills with detail.
//
// Structure (body-fixed frame, +Y is the rotation pole, latitude = asin(y)):
//   - three-cell circulation: a convergence zone (ITCZ) that follows the
//     subsolar latitude through the seasons, with organised deep convection;
//     dry subtropical subsidence with shallow trade cumulus; mid-latitude
//     storm tracks; polar low stratus;
//   - baroclinic waves along each storm track: a meandering frontal band whose
//     strength peaks in the troughs, so cyclones and fronts form real
//     synoptic-scale shapes (hundreds to thousands of km across);
//   - the climate authority (humidity, precipitation) modulates all of it, so
//     oceans, deserts and mountain rain shadows leave their mark;
//   - domain-warped multi-octave noise breaks the bands into natural shapes.
struct WeatherParameters
{
    // Latitude of the sub-stellar point; moves the circulation with the seasons.
    f64 subsolarLatitudeRadians{0.0};
    // Number of waves around each storm track.
    f64 stormWaveNumber{6.0};
    // Frequency scale of the breakup noise; the layer's weatherScale.
    f64 noiseScale{3.5};
    // Frequency scale of the fine breakup; the layer's detailScale.
    f64 detailScale{14.0};
    u64 seed{1};
};

struct WeatherClimate
{
    f64 temperatureC{15.0};
    f64 humidity{0.5};
    f64 precipitation{0.4};
    bool valid{false};
};

struct WeatherState
{
    // Sky coverage [0, 1] before the layer's coverage bias is applied.
    f64 coverage{0.0};
    // Position on the cloud type axis: 0.05 stratus, 0.2 stratocumulus,
    // 0.32 nimbostratus, 0.5 cumulus, 0.72 cumulus congestus, 1.0 cumulonimbus.
    f64 cloudType{0.5};
    // High cloud coverage [0, 1]: cirrus streaks along the jet and warm fronts,
    // thin tropical cirrus, and cumulonimbus anvil outflow drifting downwind of
    // deep convection.
    f64 cirrus{0.0};
    // Precipitation intensity [0, 1]; thickens and darkens the cloud base.
    f64 precipitation{0.0};
};

[[nodiscard]] WeatherState EvaluateWeather(
    const math::Double3& unitDirection,
    f64 seconds,
    const WeatherParameters& parameters,
    const WeatherClimate& climate) noexcept;
} // namespace orbit::celestial_clouds
