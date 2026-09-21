#include <orbit/celestial_lighting/CelestialLighting.hpp>

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

    return FailCode(0);
}
