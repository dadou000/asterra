#include <orbit/celestial_atmosphere/AtmosphereRefraction.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

using namespace orbit;
using namespace orbit::celestial_atmosphere;

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
bool Near(double a, double b, double rel) { return std::abs(a - b) <= rel * std::max(std::abs(a), std::abs(b)) + 1.0e-300; }
constexpr double kDeg = std::numbers::pi / 180.0;
// Direction at elevation `e` above the local horizon, azimuth east of north, with up = +y, north = +z, east = +x.
math::Double3 At(double elevation, double azimuth)
{
    return {std::cos(elevation) * std::sin(azimuth), std::sin(elevation), std::cos(elevation) * std::cos(azimuth)};
}
} // namespace

int main()
{
    const AtmosphereParameters earth;   // the engine's Earth-like defaults
    const auto a = RefractionAtmosphereFor(earth);
    Check(a.planetRadius == earth.bottomRadiusMeters && a.scaleHeight == earth.rayleighScaleHeightMeters, "radius and scale height come from the atmosphere");
    Check(Near(a.surfaceRefractivity, 2.78e-4, 1.0e-9), "the default atmosphere maps to Earth's refractivity");

    AtmosphereParameters dense = earth;
    dense.rayleighScatteringPerMeter = {earth.rayleighScatteringPerMeter.x * 2.0, earth.rayleighScatteringPerMeter.y * 2.0, earth.rayleighScatteringPerMeter.z * 2.0};
    Check(Near(RefractionAtmosphereFor(dense).surfaceRefractivity, 2.0 * a.surfaceRefractivity, 1.0e-9), "twice the Rayleigh scattering is twice the refractivity");

    const auto& table = RefractionTableFor(earth);
    Check(table.Valid(), "the default atmosphere has a valid refraction table");
    Check(&RefractionTableFor(earth) == &table, "the table is cached for unchanged parameters");

    const math::Double3 up{0.0, 1.0, 0.0};

    // A sun exactly at the true horizon is seen about 29 arcminutes above it, and nothing leaves the vertical plane.
    for (const double azimuth : {0.0, 1.0, 2.5})
    {
        const auto seen = RefractDirection(table, up, At(0.0, azimuth), 2.0);
        const double elevation = std::asin(seen.direction.y) / kDeg * 60.0;
        Check(seen.visible && elevation > 25.0 && elevation < 33.0, "a sun on the true horizon is seen ~29 arcminutes above it");
        const auto flat = math::Normalize(math::Double3{seen.direction.x, 0.0, seen.direction.z});
        Check(Near(flat.x, std::sin(azimuth), 1.0e-9) && Near(flat.z, std::cos(azimuth), 1.0e-9), "refraction keeps the azimuth");
        Check(Near(math::Length(seen.direction), 1.0, 1.0e-12), "the apparent direction is a unit vector");
    }

    // A sun 0.3 degrees BELOW the horizon is still seen: sunset lasts longer than geometry says.
    const auto still = RefractDirection(table, up, At(-0.3 * kDeg, 0.7), 2.0);
    Check(still.visible && still.direction.y > 0.0, "a sun 0.3 degrees below the horizon is still seen above it");
    // ...but not one 1 degree below.
    const auto gone = RefractDirection(table, up, At(-1.0 * kDeg, 0.7), 2.0);
    Check(!gone.visible && Near(gone.direction.y, std::sin(-1.0 * kDeg), 1.0e-9), "a sun 1 degree below the horizon stays hidden and the true direction is returned");

    // Overhead is unchanged; high sun moves by seconds of arc only.
    const auto zenith = RefractDirection(table, up, up, 2.0);
    Check(zenith.visible && Near(zenith.direction.y, 1.0, 1.0e-12), "straight overhead needs no bending");
    const auto high = RefractDirection(table, up, At(60.0 * kDeg, 0.3), 2.0);
    Check(std::abs(std::asin(high.direction.y) - 60.0 * kDeg) < 2.0 * kDeg / 60.0, "a sun 60 degrees up moves by under 2 arcminutes");
    Check(high.verticalCompression > 0.995, "a high sun is not flattened");
    const auto low = RefractDirection(table, up, At(0.0, 0.0), 2.0);
    Check(low.verticalCompression > 0.80 && low.verticalCompression < 0.90, "a sun on the horizon is flattened by ~15 percent");

    // A tilted local vertical (observer anywhere on the planet) works the same: rotate the whole frame.
    const math::Double3 tiltedUp = math::Normalize(math::Double3{0.3, 0.9, -0.2});
    const math::Double3 side = math::Normalize(math::Cross(tiltedUp, math::Double3{0.0, 0.0, 1.0}));
    const math::Double3 toObject = math::Normalize(side * std::cos(0.2 * kDeg) + tiltedUp * std::sin(0.2 * kDeg));
    const auto tilted = RefractDirection(table, tiltedUp, toObject, 2.0);
    Check(tilted.visible && math::Dot(tilted.direction, tiltedUp) > math::Dot(toObject, tiltedUp), "bending raises the object toward the local vertical in any frame");

    // Ducting (Venus-class) and an empty table return the true direction untouched.
    AtmosphereParameters venus = earth;
    venus.bottomRadiusMeters = 6.05e6;
    venus.topRadiusMeters = 6.05e6 + 150000.0;
    venus.rayleighScaleHeightMeters = 15900.0;
    venus.rayleighScatteringPerMeter = {earth.rayleighScatteringPerMeter.x * 15.0, earth.rayleighScatteringPerMeter.y * 15.0, earth.rayleighScatteringPerMeter.z * 15.0};
    const auto& trapped = RefractionTableFor(venus);
    const auto venusSeen = RefractDirection(trapped, up, At(0.0, 0.0), 0.0);
    Check(!trapped.Valid() && venusSeen.visible && Near(venusSeen.direction.y, 0.0, 1.0e-12), "a ducting atmosphere leaves directions unchanged");

    if (failures == 0) std::printf("AtmosphereRefraction: all tests passed\n");
    return failures;
}
