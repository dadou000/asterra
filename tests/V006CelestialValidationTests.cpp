#include <orbit/celestial_compact_objects/CompactObject.hpp>
#include <orbit/celestial_giants/GiantAppearance.hpp>
#include <orbit/celestial_magnetosphere/Magnetosphere.hpp>
#include <orbit/celestial_representation/RepresentationResolver.hpp>
#include <orbit/celestial_scheduler/CelestialWorkScheduler.hpp>
#include <orbit/celestial_small_bodies/SmallBodyAppearance.hpp>

#include <cmath>

int main()
{
    using namespace orbit;

    {
        celestial_compact_objects::CompactObjectParameters p{
            .gravitationalParameterM3PerS2 =
                1.3271645321e20
        };

        const auto s =
            celestial_compact_objects::
                ResolveScales(p);

        const f64 rg =
            s.gravitationalRadiusMeters;

        if (std::abs(
                s.schwarzschildRadiusMeters /
                    rg -
                2.0) >
            1.0e-12 ||
            std::abs(
                s.photonSphereRadiusMeters /
                    rg -
                3.0) >
            1.0e-12 ||
            std::abs(
                s.iscoRadiusMeters /
                    rg -
                6.0) >
            1.0e-12)
        {
            return 1;
        }
    }

    {
        const auto resolveAt =
            [](const f64 projectedPixels)
            {
                constexpr f64 radius =
                    1'000'000.0;
                constexpr f64 fov = 1.0;
                constexpr f64 height =
                    1000.0;

                const f64 angular =
                    projectedPixels *
                    fov /
                    height;

                return celestial_representation::
                    Resolve({
                        .bodyRadiusMeters =
                            radius,
                        .cameraDistanceToCenterMeters =
                            radius /
                            std::sin(angular),
                        .verticalFieldOfViewRadians =
                            fov,
                        .viewportHeightPixels =
                            height,
                        .features = {
                            .productionSurfaceAvailable =
                                false,
                            .macroDisplacementAvailable =
                                false,
                            .complexFarAppearance =
                                false,
                            .radiativeEmitter =
                                false
                        }
                    });
            };

        if (resolveAt(20.0).representation !=
                celestial_representation::
                    Representation::
                        SmoothGlobe ||
            resolveAt(2.0).representation !=
                celestial_representation::
                    Representation::
                        AnalyticDiscImpostor ||
            resolveAt(0.2).representation !=
                celestial_representation::
                    Representation::
                        PointProxy)
        {
            return 2;
        }
    }

    {
        celestial_giants::
            GiantAppearanceParameters p{};

        const auto a =
            celestial_giants::
                GiantAppearanceFingerprint(
                    p,
                    {.faceResolution = 17U});

        const auto b =
            celestial_giants::
                GiantAppearanceFingerprint(
                    p,
                    {.faceResolution = 17U});

        if (a == 0U || a != b)
            return 3;
    }

    {
        celestial_small_bodies::
            SmallBodyParameters p{};

        const auto a =
            celestial_small_bodies::
                SmallBodyAppearanceFingerprint(
                    p,
                    {.faceResolution = 17U});

        const auto b =
            celestial_small_bodies::
                SmallBodyAppearanceFingerprint(
                    p,
                    {.faceResolution = 17U});

        if (a == 0U || a != b)
            return 4;

        const auto shape =
            celestial_small_bodies::
                BuildSmallBodyShape(
                    p,
                    {.faceResolution = 17U});

        if (shape.fingerprint != a ||
            shape.radiusScale.size() !=
                6U * 17U * 17U ||
            !std::isfinite(
                shape.minimumRadiusScale) ||
            !std::isfinite(
                shape.maximumRadiusScale) ||
            shape.minimumRadiusScale <= 0.18 ||
            shape.maximumRadiusScale <=
                shape.minimumRadiusScale)
        {
            return 5;
        }

        const auto opposition =
            celestial_small_bodies::
                EvaluateRoughSurfacePhotometry(
                    p,
                    {
                        .normal = {0.0, 0.0, 1.0},
                        .lightDirection = {0.0, 0.0, 1.0},
                        .viewDirection = {0.0, 0.0, 1.0}
                    });

        const auto offOpposition =
            celestial_small_bodies::
                EvaluateRoughSurfacePhotometry(
                    p,
                    {
                        .normal = {0.0, 0.0, 1.0},
                        .lightDirection = {0.0, 0.0, 1.0},
                        .viewDirection = {0.5, 0.0, 0.8660254037844386}
                    });

        if (!std::isfinite(opposition) ||
            !std::isfinite(offOpposition) ||
            opposition <= offOpposition ||
            offOpposition <= 0.0)
        {
            return 6;
        }
    }

    {
        celestial_magnetosphere::
            MagnetosphereParameters p{};

        constexpr f64 radius =
            6'371'000.0;

        const auto fp =
            celestial_magnetosphere::
                MagnetosphereFingerprint(
                    p,
                    radius);

        const f64 nose =
            celestial_magnetosphere::
                MagnetopauseRadiusMeters(
                    p,
                    radius,
                    {1.0, 0.0, 0.0});

        if (fp == 0U ||
            !std::isfinite(nose) ||
            nose <= radius)
        {
            return 5;
        }
    }

    {
        using namespace
            celestial_scheduler;

        CelestialWorkScheduler scheduler({
            .maxCpuJobsPerFrame = 1U,
            .maxGpuJobsPerFrame = 1U,
            .maxCpuCostUnitsPerFrame = 2U,
            .maxGpuCostUnitsPerFrame = 2U,
            .maxPendingRequests = 8U
        });

        const WorkKey key{
            .subjectHigh = 1U,
            .subjectLow = 2U,
            .kind =
                WorkKind::
                    OrbitalAppearance
        };

        scheduler.Enqueue({
            .key = key,
            .authorityRevision = 10U,
            .backend =
                WorkBackend::Cpu,
            .costUnits = 1U,
            .priority = 1,
            .visible = true
        });

        const auto first =
            scheduler.BuildFramePlan();

        if (first.size() != 1U)
            return 6;

        scheduler.Enqueue({
            .key = key,
            .authorityRevision = 11U,
            .backend =
                WorkBackend::Cpu,
            .costUnits = 1U,
            .priority = 2,
            .visible = true
        });

        if (scheduler.Complete(
                key,
                10U))
        {
            return 7;
        }

        const auto second =
            scheduler.BuildFramePlan();

        if (second.size() != 1U ||
            second.front().
                    authorityRevision !=
                11U)
        {
            return 8;
        }
    }

    return 0;
}
