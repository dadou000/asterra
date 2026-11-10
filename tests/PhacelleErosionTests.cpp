#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/world/Planet.hpp>
#include "../engine/terrain/src/PhacelleErosion.hpp"
#include <array>
#include <cmath>
#include <iostream>

int main()
{
    using namespace orbit;
    const world::PlanetDefinition planet{.radiusMeters = 6'000'000.0};
    terrain::AnalyticTerrainDesc baseline{.seed = 0xA57E22AULL};
    const terrain::AnalyticTerrainSource original(planet, baseline);
    baseline.phacelle.strength = 0.85;
    baseline.phacelle.octaves = 4;
    baseline.phacelle.lowlandFadeMeters = 0.0;
    const terrain::AnalyticTerrainSource eroded(planet, baseline);
    const terrain::AnalyticTerrainSource repeated(planet, baseline);
    if (eroded.Revision() == original.Revision() ||
        repeated.Revision() != eroded.Revision())
    {
        std::cerr << "Phacelle recipe revision hashing failed.\n";
        return 1;
    }

    // Analytic kernel checks: zero is a strict no-op and distant footprints
    // skip sub-Nyquist features without changing the original terrain.
    const std::array<double, 3> unit{0.320001, 0.630001, 0.707177};
    auto recipe = baseline.phacelle;
    recipe.strength = 0.0;
    if (terrain::detail::PhacelleErosionDeltaMeters(unit, planet.radiusMeters, 20., recipe) != 0.)
        return 2;
    recipe.strength = 0.85;
    const auto fine = terrain::detail::PhacelleErosionDeltaMeters(unit, planet.radiusMeters, 20., recipe);
    if (!std::isfinite(fine) ||
        fine != terrain::detail::PhacelleErosionDeltaMeters(unit, planet.radiusMeters, 20., recipe) ||
        terrain::detail::PhacelleErosionDeltaMeters(unit, planet.radiusMeters, 20'000., recipe) != 0.)
    {
        std::cerr << "Phacelle finite/deterministic/footprint checks failed.\n";
        return 3;
    }

    // Verify a positive mountain modifier exists on the canonical source
    // while its high-footprint parent remains unchanged.
    bool observedChange = false;
    constexpr double pi = 3.1415926535897932384626;
    for (int i = 0; i < 192; ++i)
    {
        const double y = 1.0 - 2.0 * (i + .5) / 192.;
        const double angle = i * pi * (3.0 - std::sqrt(5.0));
        const double xz = std::sqrt(1.0 - y * y);
        const math::Double3 d{xz * std::cos(angle), y, xz * std::sin(angle)};
        const double a = eroded.Sample({d, 50.}).elevationMeters;
        const double b = original.Sample({d, 50.}).elevationMeters;
        const double c = repeated.Sample({d, 50.}).elevationMeters;
        if (!std::isfinite(a) || a != c || std::abs(a) > 20'000.)
            return 4;
        observedChange |= std::abs(a-b) > 0.01;
        const double coarse = eroded.Sample({d, 20'000.}).elevationMeters;
        const double coarseLegacy = original.Sample({d, 20'000.}).elevationMeters;
        if (std::abs(coarse-coarseLegacy) > 0.000001)
        {
            std::cerr << "High-footprint terrain no longer matches legacy field.\n";
            return 5;
        }
    }
    if (!observedChange)
    {
        std::cerr << "Phacelle never affected the sampled mountain field.\n";
        return 6;
    }
    std::cout << "Phacelle canonical source / continuity checks passed.\n";
    return 0;
}
