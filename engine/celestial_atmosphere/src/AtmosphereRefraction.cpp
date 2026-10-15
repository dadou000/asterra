#include <orbit/celestial_atmosphere/AtmosphereRefraction.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbit::celestial_atmosphere
{
optical_transport::RefractionAtmosphere RefractionAtmosphereFor(const AtmosphereParameters& p)
{
    constexpr f64 kEarthGreenRayleigh = 13.558e-6;   // AtmosphereParameters{} default
    optical_transport::RefractionAtmosphere a;
    a.planetRadius = p.bottomRadiusMeters;
    a.scaleHeight = std::max(p.rayleighScaleHeightMeters, 1.0);
    a.surfaceRefractivity = optical_transport::SurfaceRefractivityFromSolver(
        std::max(p.rayleighScatteringPerMeter.y, 0.0) / kEarthGreenRayleigh, 1.0);
    return a;
}

const optical_transport::RefractionTable& RefractionTableFor(const AtmosphereParameters& parameters)
{
    struct Cache
    {
        optical_transport::RefractionAtmosphere key{};
        optical_transport::RefractionTable table;
        bool filled = false;
    };
    static thread_local Cache cache;
    const auto a = RefractionAtmosphereFor(parameters);
    if (!cache.filled || cache.key.planetRadius != a.planetRadius || cache.key.surfaceRefractivity != a.surfaceRefractivity ||
        cache.key.scaleHeight != a.scaleHeight)
    {
        cache.key = a;
        cache.table = optical_transport::RefractionTable::Build(a, std::max(parameters.topRadiusMeters - parameters.bottomRadiusMeters, 1000.0));
        cache.filled = true;
    }
    return cache.table;
}

ApparentDirection RefractDirection(
    const optical_transport::RefractionTable& table, const math::Double3& up, const math::Double3& toObject, const f64 altitude)
{
    ApparentDirection out;
    out.direction = toObject;
    if (!table.Valid())
    {
        return out;
    }
    const f64 sinElevation = std::clamp(math::Dot(up, toObject), -1.0, 1.0);
    const f64 trueElevation = std::asin(sinElevation);
    f64 apparent = 0.0;
    if (!table.ApparentElevation(trueElevation, std::max(altitude, 0.0), apparent))
    {
        out.visible = false;
        return out;
    }
    out.verticalCompression = table.VerticalCompression(trueElevation, std::max(altitude, 0.0));
    // Rotate in the vertical plane (the plane of `up` and `toObject`) by the bending, toward the zenith.
    const math::Double3 horizontalRaw = toObject - up * sinElevation;
    const f64 horizontalLength = math::Length(horizontalRaw);
    if (horizontalLength < 1.0e-9)
    {
        return out;   // straight up or down: no bending
    }
    const math::Double3 horizontal = horizontalRaw / horizontalLength;
    out.direction = math::Normalize(horizontal * std::cos(apparent) + up * std::sin(apparent));
    return out;
}
} // namespace orbit::celestial_atmosphere
