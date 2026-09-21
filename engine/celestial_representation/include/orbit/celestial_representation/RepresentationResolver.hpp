#pragma once

#include <orbit/core/Types.hpp>

#include <optional>
#include <string_view>

namespace orbit::celestial_representation
{
enum class Representation : u8
{
    ProductionSurface = 0,
    MacroDisplacedGlobe = 1,
    SmoothGlobe = 2,
    AnalyticDiscImpostor = 3,
    CachedDiscImpostor = 4,
    PointProxy = 5,
    StellarPointProxy = 6
};

struct FeatureRequirements
{
    bool productionSurfaceAvailable{false};
    bool macroDisplacementAvailable{false};
    bool complexFarAppearance{false};
    bool radiativeEmitter{false};
};

struct QualityPolicy
{
    // Maximum acceptable projected geometric error before promoting.
    f64 productionSurfaceErrorPixels{2.0};
    f64 macroDisplacementErrorPixels{0.45};

    // Apparent-radius thresholds for geometric representation changes.
    f64 smoothGlobeMinimumRadiusPixels{10.0};
    f64 discImpostorMinimumRadiusPixels{0.55};

    // Global quality multiplier: >1 retains richer representations longer.
    f64 qualityScale{1.0};

    // Relative hysteresis applied around all thresholds.
    f64 hysteresisFraction{0.15};
};

struct ResolveInput
{
    f64 bodyRadiusMeters{1.0};
    f64 maximumProductionDetailMeters{1.0};
    f64 maximumMacroDisplacementMeters{0.0};
    f64 cameraDistanceToCenterMeters{1.0};
    f64 verticalFieldOfViewRadians{1.0};
    f64 viewportHeightPixels{1080.0};
    FeatureRequirements features{};
    QualityPolicy policy{};
    std::optional<Representation> previous{};
};

struct Decision
{
    Representation representation{Representation::PointProxy};
    Representation lowerFidelityNeighbor{Representation::PointProxy};
    f64 blendToLower{0.0};

    f64 projectedRadiusPixels{0.0};
    f64 productionDetailErrorPixels{0.0};
    f64 macroDisplacementErrorPixels{0.0};

    bool hysteresisHeld{false};
};

struct SurfaceGlobeTransition
{
    f64 productionSurfaceWeight{0.0};
    f64 macroGlobeWeight{0.0};
    f64 transitionToGlobe{0.0};
    bool overlapping{false};
};

[[nodiscard]] Decision Resolve(
    const ResolveInput& input);

[[nodiscard]] SurfaceGlobeTransition
ResolveSurfaceGlobeTransition(
    const ResolveInput& input,
    const Decision& decision);

[[nodiscard]] std::string_view Name(
    Representation representation) noexcept;
} // namespace orbit::celestial_representation
