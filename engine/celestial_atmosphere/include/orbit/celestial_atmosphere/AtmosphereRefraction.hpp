#pragma once

#include <orbit/celestial_atmosphere/Atmosphere.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/optical_transport/Refraction.hpp>

namespace orbit::celestial_atmosphere
{
// Refraction of an authored atmosphere, derived entirely from AtmosphereParameters (no new authored property):
//   radius          = bottomRadiusMeters
//   scale height    = rayleighScaleHeightMeters
//   refractivity    = Earth reference (2.78e-4 at 550 nm) x (green Rayleigh scattering / Earth's 13.558e-6 per metre).
// The last line assumes an Earth-like gas: Rayleigh scattering goes as N a^2 and refractivity as N a, so with an unknown
// polarizability the density ratio carries both. A planet with a very different gas mix should author the refractivity
// explicitly (SurfaceRefractivityFromSolver takes the solver's composition factor).
[[nodiscard]] optical_transport::RefractionAtmosphere RefractionAtmosphereFor(const AtmosphereParameters& parameters);

// Table for the atmosphere, built once and cached for the most recent parameters (building takes ~50 ms).
[[nodiscard]] const optical_transport::RefractionTable& RefractionTableFor(const AtmosphereParameters& parameters);

struct ApparentDirection
{
    math::Double3 direction{};   // unit vector toward where the object is SEEN
    bool visible = true;         // false when the planet hides it even after refraction (direction is then the true one)
    f64 verticalCompression = 1.0;   // d(apparent)/d(true) elevation: a disc is this much shorter than wide
};

// Where an object at true direction `toObject` (unit, same frame as `up`) is seen by an observer whose local vertical is
// `up` (unit) at `observerAltitudeMeters`: rotated toward `up` by the bending. Observers in space, and ducting
// atmospheres, get the true direction unchanged.
[[nodiscard]] ApparentDirection RefractDirection(
    const optical_transport::RefractionTable& table,
    const math::Double3& up,
    const math::Double3& toObject,
    f64 observerAltitudeMeters);
} // namespace orbit::celestial_atmosphere
