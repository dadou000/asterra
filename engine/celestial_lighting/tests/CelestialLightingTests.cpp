#include <orbit/celestial_lighting/CelestialLighting.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>
#include <iostream>

namespace
{
bool Near(
    const double a,
    const double b,
    const double tolerance = 1.0e-9)
{
    return
        std::abs(a - b) <=
        tolerance *
        std::max({
            1.0,
            std::abs(a),
            std::abs(b)
        });
}
}

namespace
{
int FailCode(const int code)
{
    std::cerr << "celestial-lighting failure code " << code << '\n';
    return code;
}
} // namespace

int main()
{
    using namespace orbit::celestial_lighting;

    const ApparentDisc source{
        .centerFromObserverMeters = {
            0.0, 0.0, 1000.0},
        .radiusMeters = 100.0
    };

    const auto clear =
        FiniteDiscOccultation(
            source,
            ApparentDisc{
                .centerFromObserverMeters = {
                    500.0, 0.0, 500.0},
                .radiusMeters = 10.0
            });

    if (clear.overlapping ||
        clear.visibleFraction != 1.0)
    {
        return FailCode(1);
    }

    const auto total =
        FiniteDiscOccultation(
            source,
            ApparentDisc{
                .centerFromObserverMeters = {
                    0.0, 0.0, 500.0},
                .radiusMeters = 100.0
            });

    if (!total.total ||
        total.visibleFraction >
            1.0e-12)
    {
        return FailCode(2);
    }

    const auto annular =
        FiniteDiscOccultation(
            source,
            ApparentDisc{
                .centerFromObserverMeters = {
                    0.0, 0.0, 500.0},
                .radiusMeters = 20.0
            });

    if (!annular.annular ||
        !(annular.visibleFraction > 0.0) ||
        !(annular.visibleFraction < 1.0))
    {
        return FailCode(3);
    }

    const auto behind =
        FiniteDiscOccultation(
            source,
            ApparentDisc{
                .centerFromObserverMeters = {
                    0.0, 0.0, 2000.0},
                .radiusMeters = 500.0
            });

    if (behind.occluderInFront ||
        behind.overlapping)
    {
        return FailCode(4);
    }

    const auto combined =
        CombinedOccultation(
            source,
            {
                ApparentDisc{
                    .centerFromObserverMeters = {
                        0.0, 0.0, 500.0},
                    .radiusMeters = 20.0},
                ApparentDisc{
                    .centerFromObserverMeters = {
                        500.0, 0.0, 500.0},
                    .radiusMeters = 10.0}
            });

    if (combined.contributingOccluders !=
            1U ||
        !Near(
            combined.visibleFraction,
            annular.visibleFraction))
    {
        return FailCode(5);
    }

    const auto duplicateUnion =
        CombinedOccultation(
            source,
            {
                ApparentDisc{
                    .centerFromObserverMeters = {
                        0.0, 0.0, 500.0},
                    .radiusMeters = 20.0},
                ApparentDisc{
                    .centerFromObserverMeters = {
                        0.0, 0.0, 500.0},
                    .radiusMeters = 20.0}
            });

    if (duplicateUnion.contributingOccluders != 2U ||
        std::abs(
            duplicateUnion.visibleFraction -
            annular.visibleFraction) >
            0.02)
    {
        return FailCode(6);
    }

    const auto direct =
        AttenuatedDirectIrradiance(
            3.828e26,
            149'597'870'700.0,
            0.25);

    if (!(direct.unoccludedWattsPerSquareMeter >
            1359.0) ||
        !(direct.unoccludedWattsPerSquareMeter <
            1363.0) ||
        !Near(
            direct.irradianceWattsPerSquareMeter,
            direct.unoccludedWattsPerSquareMeter *
                0.25))
    {
        return FailCode(7);
    }

    if (!Near(
            LambertPhase(0.0),
            1.0) ||
        !Near(
            LambertPhase(
                std::numbers::pi_v<double>),
            0.0,
            1.0e-8))
    {
        return FailCode(8);
    }

    const auto quarter =
        LambertSphereReflectedLight(
            1000.0,
            1.0e6,
            1.0e8,
            std::numbers::pi_v<double> *
                0.5);

    if (!(quarter.phaseFunction > 0.0) ||
        !(quarter.phaseFunction < 1.0) ||
        !(quarter.unitGeometricAlbedoIrradianceWattsPerSquareMeter >
            0.0))
    {
        return FailCode(9);
    }

    // ---- limb darkening: shares of emitted light, checked against an independent planar quadrature ----
    {
        constexpr double sourceDistance = 1.495978707e11;
        constexpr double sourceRadius = 6.957e8;
        const double a = std::asin(sourceRadius / sourceDistance);   // source angular radius
        constexpr double occluderDistance = 3.844e8;

        // Occluder disc whose centre sits at offset (ox, oy) source radii from the source centre, radius rho source
        // radii (small angles: the planar approximation is exact to ~1e-5 here).
        const auto occluderAt = [&](const double ox, const double oy, const double rho) {
            return ApparentDisc{
                .centerFromObserverMeters = {ox * a * occluderDistance, oy * a * occluderDistance, occluderDistance},
                .radiusMeters = rho * a * occluderDistance};
        };
        const auto sourceWith = [&](const double u) {
            return ApparentDisc{.centerFromObserverMeters = {0.0, 0.0, sourceDistance}, .radiusMeters = sourceRadius,
                                .limbDarkening = u};
        };
        // Reference: 1200 x 1200 midpoint rule over the unit disc, weight 1 - u (1 - sqrt(1 - r^2)).
        struct Circle { double x, y, r; };
        const auto reference = [](const double u, const std::vector<Circle>& circles) {
            constexpr int n = 1200;
            double covered = 0.0, total = 0.0;
            for (int i = 0; i < n; ++i)
            {
                for (int j = 0; j < n; ++j)
                {
                    const double x = -1.0 + (2.0 * i + 1.0) / n, y = -1.0 + (2.0 * j + 1.0) / n;
                    const double r2 = x * x + y * y;
                    if (r2 > 1.0) continue;
                    const double w = 1.0 - u * (1.0 - std::sqrt(1.0 - r2));
                    total += w;
                    for (const Circle& c : circles)
                    {
                        if ((x - c.x) * (x - c.x) + (y - c.y) * (y - c.y) <= c.r * c.r) { covered += w; break; }
                    }
                }
            }
            return covered / total;
        };

        // u = 0 keeps the analytic area fraction exactly.
        const auto uniform = FiniteDiscOccultation(sourceWith(0.0), occluderAt(0.0, 0.0, 0.4));
        if (!Near(uniform.obscuredFraction, 0.16, 1.0e-4)) return FailCode(10);

        // A small occluder over the bright centre hides more light than area; one on the dim limb hides less.
        const double u = 0.58;
        const auto centre = FiniteDiscOccultation(sourceWith(u), occluderAt(0.0, 0.0, 0.4));
        const double centreRef = reference(u, {{0.0, 0.0, 0.4}});
        if (!(centre.obscuredFraction > 0.16 + 0.03) || !Near(centre.obscuredFraction, centreRef, 5.0e-3)) return FailCode(11);
        const auto limb = FiniteDiscOccultation(sourceWith(u), occluderAt(0.95, 0.0, 0.3));
        const auto limbUniform = FiniteDiscOccultation(sourceWith(0.0), occluderAt(0.95, 0.0, 0.3));
        if (!(limb.obscuredFraction < limbUniform.obscuredFraction - 0.004) ||
            !Near(limb.obscuredFraction, reference(u, {{0.95, 0.0, 0.3}}), 5.0e-3)) return FailCode(12);
        if (!Near(limb.visibleFraction, 1.0 - limb.obscuredFraction, 1.0e-12)) return FailCode(13);

        // No overlap and total eclipse are unchanged by darkening.
        if (FiniteDiscOccultation(sourceWith(u), occluderAt(3.0, 0.0, 0.3)).obscuredFraction != 0.0) return FailCode(14);
        const auto total = FiniteDiscOccultation(sourceWith(u), occluderAt(0.0, 0.0, 1.5));
        if (!total.total || !Near(total.obscuredFraction, 1.0, 1.0e-12)) return FailCode(15);

        // Two overlapping occluders: the union, not the product of their separate visibilities.
        const std::vector<ApparentDisc> pair{occluderAt(0.15, 0.0, 0.5), occluderAt(0.25, 0.1, 0.5)};
        const auto both = CombinedOccultation(sourceWith(u), pair);
        const double unionRef = reference(u, {{0.15, 0.0, 0.5}, {0.25, 0.1, 0.5}});
        if (!Near(both.obscuredFraction, unionRef, 6.0e-3)) return FailCode(16);
        const double productVisible = (1.0 - FiniteDiscOccultation(sourceWith(u), pair[0]).obscuredFraction) *
                                      (1.0 - FiniteDiscOccultation(sourceWith(u), pair[1]).obscuredFraction);
        if (!(both.visibleFraction > productVisible + 0.03)) return FailCode(17);
    }

    return FailCode(0);
}
