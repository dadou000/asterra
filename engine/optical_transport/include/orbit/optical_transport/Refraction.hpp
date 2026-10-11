#pragma once

#include <cstddef>
#include <vector>

namespace orbit::optical_transport
{

// Planetary atmospheric refraction for an exponential atmosphere, n(h) = 1 + N0 exp(-h / H) on a sphere of radius R,
// from the three numbers every authored atmosphere already has (radius, surface refractivity, scale height).
//
// Rays are integrated through concentric shells with Snell's invariant n r sin(z) = const (no flat-earth shortcut, no
// Earth constants), so thin Mars-like and dense Venus-like atmospheres work as well as Earth. Local detail (inversions,
// mirages, heat shimmer) is not modelled: it needs a temperature profile; see CriticalGradientRatio for when it matters.
struct RefractionAtmosphere
{
    double planetRadius = 6371000.0;        // m
    double surfaceRefractivity = 2.78e-4;   // n - 1 at the surface, for the wavelength of interest
    double scaleHeight = 8434.0;            // m (refractivity scale height, ~ density scale height)
};

// Surface refractivity from the quantities the atmosphere solver already derives. Rayleigh scattering goes as N a^2 and
// refractivity as N a (a = molecular polarizability), so relative to a reference atmosphere
//   (n - 1) = (n - 1)_ref * (number density / reference) * sqrt(Rayleigh composition factor / reference).
// The reference values are an Earth-like surface at 550 nm.
double SurfaceRefractivityFromSolver(double numberDensityRatio, double rayleighCompositionRatio,
                                     double referenceRefractivity = 2.78e-4);

// n - 1 at height h.
double RefractivityAt(const RefractionAtmosphere& atmosphere, double height);

// Closed-form refraction at the horizon for an observer at the surface, N0 sqrt(pi R / (2 H)) (radians); an independent
// cross-check of the numerical integral.
double HorizonRefractionEstimate(const RefractionAtmosphere& atmosphere);

class RefractionTable
{
public:
    // maxObserverAltitude: highest observer height covered (clamped above). Rows are log-spaced in altitude.
    static RefractionTable Build(const RefractionAtmosphere& atmosphere, double maxObserverAltitude = 120000.0);

    // True when the surface refractivity gradient is steeper than the planet's curvature (Venus-class atmospheres):
    // horizon rays are trapped and refraction is not a single-valued function of elevation. The table is then empty,
    // Valid() is false and every query returns false / 0; such atmospheres need ray tracing.
    static bool Ducting(const RefractionAtmosphere& atmosphere);
    bool Valid() const { return !rows_.empty(); }

    // Elevations are angles above the observer's local horizontal plane, radians.
    // Apparent elevation of an object whose geometric (airless) elevation is `trueElevation`. Returns false when the
    // planet blocks the ray (the object is below the refracted limb); `apparent` is then unchanged.
    bool ApparentElevation(double trueElevation, double observerAltitude, double& apparent) const;
    // Inverse: the geometric elevation of an object seen at `apparentElevation`.
    bool TrueElevation(double apparentElevation, double observerAltitude, double& trueElev) const;
    // Bending apparent - true (>= 0 in an ordinary atmosphere), radians; 0 if blocked.
    double Bending(double trueElevation, double observerAltitude) const;
    // d(apparent)/d(true): below 1 the object is squeezed vertically (a setting Sun is flattened); about 0.85 at the
    // horizon on Earth (the Sun's disc is ~15 percent shorter than wide), approaching 1 higher up.
    double VerticalCompression(double trueElevation, double observerAltitude) const;
    // Apparent elevation of the planet's limb seen from this altitude (negative: below the horizontal), radians, and the
    // airless geometric dip for comparison.
    double ApparentLimbElevation(double observerAltitude) const;
    static double GeometricDip(const RefractionAtmosphere& atmosphere, double observerAltitude);

    const RefractionAtmosphere& Atmosphere() const { return atmosphere_; }

private:
    struct Row
    {
        double altitude = 0.0;
        std::vector<double> apparent;   // ascending, from the limb to the zenith
        std::vector<double> truth;      // matching geometric elevations, ascending
    };
    RefractionAtmosphere atmosphere_;
    std::vector<Row> rows_;

    bool Lookup(const Row& row, double value, bool fromApparent, double& out) const;
    std::size_t RowBelow(double altitude, double& fraction) const;
};

// Ratio of the vertical refractivity gradient to the one that would bend a ray along the planet's surface
// (d(n)/dh = -1/R): above 1 rays are trapped (ducting, superior mirages); ordinary air is ~0.17 and the ratio crosses 1
// near a temperature inversion steeper than roughly 0.1 K/m. Local layers like this need ray tracing, not this table.
double CriticalGradientRatio(double refractivityGradientPerMeter, double planetRadius);

} // namespace orbit::optical_transport
