#include <orbit/celestial_representation/RepresentationResolver.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::celestial_representation
{
namespace
{
void Validate(const ResolveInput& input)
{
    const auto finitePositive =
        [](const f64 value)
        {
            return std::isfinite(value) &&
                   value > 0.0;
        };

    if (!finitePositive(input.bodyRadiusMeters) ||
        !finitePositive(input.cameraDistanceToCenterMeters) ||
        !finitePositive(input.verticalFieldOfViewRadians) ||
        !finitePositive(input.viewportHeightPixels) ||
        !std::isfinite(input.maximumProductionDetailMeters) ||
        input.maximumProductionDetailMeters < 0.0 ||
        !std::isfinite(input.maximumMacroDisplacementMeters) ||
        input.maximumMacroDisplacementMeters < 0.0 ||
        !finitePositive(input.policy.productionSurfaceErrorPixels) ||
        !finitePositive(input.policy.macroDisplacementErrorPixels) ||
        !finitePositive(input.policy.smoothGlobeMinimumRadiusPixels) ||
        !finitePositive(input.policy.discImpostorMinimumRadiusPixels) ||
        !finitePositive(input.policy.qualityScale) ||
        !std::isfinite(input.policy.hysteresisFraction) ||
        input.policy.hysteresisFraction < 0.0 ||
        input.policy.hysteresisFraction >= 0.5)
    {
        throw std::invalid_argument(
            "Celestial representation resolver input is invalid.");
    }
}

[[nodiscard]] f64 ProjectedRadiusPixels(
    const ResolveInput& input) noexcept
{
    if (input.cameraDistanceToCenterMeters <=
        input.bodyRadiusMeters)
    {
        return
            0.5 *
            input.viewportHeightPixels;
    }

    const f64 ratio =
        std::clamp(
            input.bodyRadiusMeters /
                input.cameraDistanceToCenterMeters,
            0.0,
            1.0);

    const f64 angularRadius =
        std::asin(ratio);

    return
        angularRadius /
        input.verticalFieldOfViewRadians *
        input.viewportHeightPixels;
}

[[nodiscard]] f64 ProjectedLinearError(
    const f64 projectedRadiusPixels,
    const f64 bodyRadiusMeters,
    const f64 featureMeters) noexcept
{
    if (featureMeters <= 0.0)
    {
        return 0.0;
    }

    return projectedRadiusPixels *
        featureMeters /
        bodyRadiusMeters;
}

[[nodiscard]] int Rank(
    const Representation representation) noexcept
{
    switch (representation)
    {
    case Representation::ProductionSurface:
        return 0;
    case Representation::MacroDisplacedGlobe:
        return 1;
    case Representation::SmoothGlobe:
        return 2;
    case Representation::CachedDiscImpostor:
    case Representation::AnalyticDiscImpostor:
        return 3;
    case Representation::StellarPointProxy:
    case Representation::PointProxy:
        return 4;
    }

    return 4;
}

[[nodiscard]] Representation DiscChoice(
    const FeatureRequirements& features) noexcept
{
    return features.complexFarAppearance
        ? Representation::CachedDiscImpostor
        : Representation::AnalyticDiscImpostor;
}

[[nodiscard]] Representation PointChoice(
    const FeatureRequirements& features) noexcept
{
    return features.radiativeEmitter
        ? Representation::StellarPointProxy
        : Representation::PointProxy;
}

[[nodiscard]] f64 ThresholdForBoundary(
    const Representation richer,
    const ResolveInput& input)
{
    const f64 quality =
        input.policy.qualityScale;

    switch (richer)
    {
    case Representation::ProductionSurface:
        return
            input.policy.productionSurfaceErrorPixels /
            quality;
    case Representation::MacroDisplacedGlobe:
        return
            input.policy.macroDisplacementErrorPixels /
            quality;
    case Representation::SmoothGlobe:
        return
            input.policy.smoothGlobeMinimumRadiusPixels /
            quality;
    case Representation::AnalyticDiscImpostor:
    case Representation::CachedDiscImpostor:
        return
            input.policy.discImpostorMinimumRadiusPixels /
            quality;
    default:
        return 0.0;
    }
}

[[nodiscard]] f64 MetricForBoundary(
    const Representation richer,
    const Decision& decision) noexcept
{
    switch (richer)
    {
    case Representation::ProductionSurface:
        return decision.productionDetailErrorPixels;
    case Representation::MacroDisplacedGlobe:
        return decision.macroDisplacementErrorPixels;
    case Representation::SmoothGlobe:
    case Representation::AnalyticDiscImpostor:
    case Representation::CachedDiscImpostor:
        return decision.projectedRadiusPixels;
    default:
        return 0.0;
    }
}

[[nodiscard]] Representation BaseResolve(
    const ResolveInput& input,
    const Decision& metrics)
{
    const f64 quality =
        input.policy.qualityScale;

    if (input.features.productionSurfaceAvailable &&
        metrics.productionDetailErrorPixels >=
            input.policy.productionSurfaceErrorPixels /
                quality)
    {
        return Representation::ProductionSurface;
    }

    if (input.features.macroDisplacementAvailable &&
        metrics.macroDisplacementErrorPixels >=
            input.policy.macroDisplacementErrorPixels /
                quality)
    {
        return Representation::MacroDisplacedGlobe;
    }

    if (metrics.projectedRadiusPixels >=
        input.policy.smoothGlobeMinimumRadiusPixels /
            quality)
    {
        return Representation::SmoothGlobe;
    }

    if (metrics.projectedRadiusPixels >=
        input.policy.discImpostorMinimumRadiusPixels /
            quality)
    {
        return DiscChoice(input.features);
    }

    return PointChoice(input.features);
}

[[nodiscard]] Representation LowerNeighbor(
    const Representation current,
    const FeatureRequirements& features) noexcept
{
    switch (current)
    {
    case Representation::ProductionSurface:
        return features.macroDisplacementAvailable
            ? Representation::MacroDisplacedGlobe
            : Representation::SmoothGlobe;
    case Representation::MacroDisplacedGlobe:
        return Representation::SmoothGlobe;
    case Representation::SmoothGlobe:
        return DiscChoice(features);
    case Representation::AnalyticDiscImpostor:
    case Representation::CachedDiscImpostor:
        return PointChoice(features);
    case Representation::PointProxy:
    case Representation::StellarPointProxy:
        return current;
    }

    return current;
}
} // namespace

Decision Resolve(
    const ResolveInput& input)
{
    Validate(input);

    Decision result;
    result.projectedRadiusPixels =
        ProjectedRadiusPixels(input);
    // A globe already represents mountain-scale displacement. Local clipmaps
    // are useful only near the surface, irrespective of how tall the mountains
    // look from orbit. Use distance above the relief envelope for the local
    // detail projection, and bound the local chart to one percent of the radius.
    const f64 surfaceDistance = std::max(
        input.cameraDistanceToCenterMeters - input.bodyRadiusMeters -
            input.maximumMacroDisplacementMeters,
        1.0);
    const f64 focalPixels = input.viewportHeightPixels /
        (2.0 * std::tan(input.verticalFieldOfViewRadians * 0.5));
    const f64 localRange = input.bodyRadiusMeters * 0.01;
    result.productionDetailErrorPixels = std::min(
        focalPixels * input.maximumProductionDetailMeters / surfaceDistance,
        input.policy.productionSurfaceErrorPixels / input.policy.qualityScale *
            localRange / surfaceDistance);
    result.macroDisplacementErrorPixels =
        ProjectedLinearError(
            result.projectedRadiusPixels,
            input.bodyRadiusMeters,
            input.maximumMacroDisplacementMeters);

    const Representation base =
        BaseResolve(input, result);

    result.representation = base;

    if (input.previous.has_value() &&
        Rank(*input.previous) != Rank(base))
    {
        const Representation richer =
            Rank(*input.previous) < Rank(base)
                ? *input.previous
                : base;

        const f64 threshold =
            ThresholdForBoundary(
                richer,
                input);

        const f64 metric =
            MetricForBoundary(
                richer,
                result);

        const f64 band =
            threshold *
            input.policy.hysteresisFraction;

        if (std::abs(metric - threshold) <= band)
        {
            result.representation =
                *input.previous;
            result.hysteresisHeld = true;
        }
    }

    result.lowerFidelityNeighbor =
        LowerNeighbor(
            result.representation,
            input.features);

    if (result.lowerFidelityNeighbor ==
        result.representation)
    {
        result.blendToLower = 0.0;
        return result;
    }

    const f64 threshold =
        ThresholdForBoundary(
            result.representation,
            input);

    const f64 metric =
        MetricForBoundary(
            result.representation,
            result);

    if (threshold <= 0.0)
    {
        result.blendToLower = 0.0;
        return result;
    }

    const f64 overlap =
        std::max(
            threshold *
                input.policy.hysteresisFraction,
            1.0e-12);

    result.blendToLower =
        std::clamp(
            (threshold + overlap - metric) /
                (2.0 * overlap),
            0.0,
            1.0);

    return result;
}

SurfaceGlobeTransition
ResolveSurfaceGlobeTransition(
    const ResolveInput& input,
    const Decision& decision)
{
    Validate(input);

    if (!input.features.productionSurfaceAvailable)
    {
        return {
            .productionSurfaceWeight = 0.0,
            .macroGlobeWeight =
                input.features.macroDisplacementAvailable
                    ? 1.0
                    : 0.0,
            .transitionToGlobe =
                input.features.macroDisplacementAvailable
                    ? 1.0
                    : 0.0,
            .overlapping = false
        };
    }

    if (!input.features.macroDisplacementAvailable)
    {
        return {
            .productionSurfaceWeight = 1.0,
            .macroGlobeWeight = 0.0,
            .transitionToGlobe = 0.0,
            .overlapping = false
        };
    }

    const f64 threshold =
        input.policy.productionSurfaceErrorPixels /
        input.policy.qualityScale;

    const f64 halfBand =
        std::max(
            threshold *
                input.policy.hysteresisFraction,
            1.0e-12);

    const f64 richerEdge =
        threshold + halfBand;
    const f64 globeEdge =
        std::max(
            threshold - halfBand,
            0.0);

    f64 transition = 0.0;

    if (decision.productionDetailErrorPixels >=
        richerEdge)
    {
        transition = 0.0;
    }
    else if (
        decision.productionDetailErrorPixels <=
        globeEdge)
    {
        transition = 1.0;
    }
    else
    {
        const f64 t =
            std::clamp(
                (richerEdge -
                 decision.productionDetailErrorPixels) /
                    std::max(
                        richerEdge - globeEdge,
                        1.0e-12),
                0.0,
                1.0);

        transition =
            t * t * (3.0 - 2.0 * t);
    }

    return {
        .productionSurfaceWeight =
            1.0 - transition,
        .macroGlobeWeight =
            transition,
        .transitionToGlobe =
            transition,
        .overlapping =
            transition > 0.0 &&
            transition < 1.0
    };
}

RepresentationBlend
ResolveRepresentationBlend(
    const ResolveInput& input,
    const Decision& decision)
{
    Validate(input);

    const auto smoothBoundary =
        [&](const Representation richer,
            const Representation lower,
            const f64 metric,
            const f64 threshold)
        {
            RepresentationBlend blend{
                .richer = richer,
                .lower = lower,
                .richerWeight = 1.0,
                .lowerWeight = 0.0,
                .overlapping = false
            };

            const f64 halfBand =
                std::max(
                    threshold *
                        input.policy.
                            hysteresisFraction,
                    1.0e-12);

            const f64 richerEdge =
                threshold + halfBand;
            const f64 lowerEdge =
                std::max(
                    threshold - halfBand,
                    0.0);

            if (metric >= richerEdge)
            {
                return blend;
            }

            if (metric <= lowerEdge)
            {
                blend.richerWeight = 0.0;
                blend.lowerWeight = 1.0;
                return blend;
            }

            const f64 t =
                std::clamp(
                    (richerEdge - metric) /
                        std::max(
                            richerEdge -
                                lowerEdge,
                            1.0e-12),
                    0.0,
                    1.0);

            const f64 smooth =
                t * t *
                (3.0 - 2.0 * t);

            blend.richerWeight =
                1.0 - smooth;
            blend.lowerWeight =
                smooth;
            blend.overlapping = true;
            return blend;
        };

    const f64 quality =
        input.policy.qualityScale;

    if (input.features.productionSurfaceAvailable &&
        input.features.macroDisplacementAvailable)
    {
        const f64 threshold =
            input.policy.
                productionSurfaceErrorPixels /
            quality;
        const f64 band =
            threshold *
            input.policy.hysteresisFraction;

        if (std::abs(
                decision.
                    productionDetailErrorPixels -
                threshold) <= band)
        {
            return smoothBoundary(
                Representation::ProductionSurface,
                Representation::MacroDisplacedGlobe,
                decision.
                    productionDetailErrorPixels,
                threshold);
        }
    }

    if (input.features.macroDisplacementAvailable)
    {
        const f64 threshold =
            input.policy.
                macroDisplacementErrorPixels /
            quality;
        const f64 band =
            threshold *
            input.policy.hysteresisFraction;

        if (std::abs(
                decision.
                    macroDisplacementErrorPixels -
                threshold) <= band)
        {
            return smoothBoundary(
                Representation::MacroDisplacedGlobe,
                Representation::SmoothGlobe,
                decision.
                    macroDisplacementErrorPixels,
                threshold);
        }
    }

    {
        const f64 threshold =
            input.policy.
                smoothGlobeMinimumRadiusPixels /
            quality;
        const f64 band =
            threshold *
            input.policy.hysteresisFraction;

        if (std::abs(
                decision.projectedRadiusPixels -
                threshold) <= band)
        {
            return smoothBoundary(
                Representation::SmoothGlobe,
                DiscChoice(input.features),
                decision.projectedRadiusPixels,
                threshold);
        }
    }

    {
        const f64 threshold =
            input.policy.
                discImpostorMinimumRadiusPixels /
            quality;
        const f64 band =
            threshold *
            input.policy.hysteresisFraction;

        if (std::abs(
                decision.projectedRadiusPixels -
                threshold) <= band)
        {
            return smoothBoundary(
                DiscChoice(input.features),
                PointChoice(input.features),
                decision.projectedRadiusPixels,
                threshold);
        }
    }

    return {
        .richer = decision.representation,
        .lower = decision.representation,
        .richerWeight = 1.0,
        .lowerWeight = 0.0,
        .overlapping = false
    };
}

std::string_view Name(
    const Representation representation) noexcept
{
    switch (representation)
    {
    case Representation::ProductionSurface:
        return "Production Surface";
    case Representation::MacroDisplacedGlobe:
        return "Macro-displaced Globe";
    case Representation::SmoothGlobe:
        return "Smooth Globe";
    case Representation::AnalyticDiscImpostor:
        return "Analytic Disc Impostor";
    case Representation::CachedDiscImpostor:
        return "Cached Disc Impostor";
    case Representation::PointProxy:
        return "Point Proxy";
    case Representation::StellarPointProxy:
        return "Stellar Point Proxy";
    }

    return "Unknown";
}
} // namespace orbit::celestial_representation
