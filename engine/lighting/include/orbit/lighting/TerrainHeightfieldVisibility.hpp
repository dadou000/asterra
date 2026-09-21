#pragma once

#include <orbit/lighting/Visibility.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/time/SimulationTime.hpp>
#include <orbit/world/Planet.hpp>

namespace orbit::lighting
{
struct TerrainVisibilityConfig
{
    // Conservative search shell around the reference sphere. This is a
    // traversal bound, not terrain authority.
    f64 maximumAbsoluteElevationMeters{20'000.0};

    f64 minimumStepMeters{0.25};
    f64 maximumStepMeters{2'000.0};
    f64 normalSampleSpacingMeters{1.0};

    u32 maximumMarchSteps{512U};
    u32 rootRefinementIterations{12U};
};

class TerrainHeightfieldVisibilityProvider final
    : public VisibilityProvider
{
public:
    TerrainHeightfieldVisibilityProvider(
        universe::BodyId body,
        world::PlanetDefinition planet,
        const terrain::TerrainSource& source,
        const universe::BodyRegistry& bodies,
        const frames::FrameGraph& frames,
        TerrainVisibilityConfig config = {},
        time::SimulationTime atTime = {});

    void SetTime(
        time::SimulationTime atTime) noexcept;

    [[nodiscard]] const VisibilityProviderDesc&
    Description() const noexcept override;

    [[nodiscard]] bool SupportsPurpose(
        VisibilityPurpose purpose) const noexcept override;

    [[nodiscard]] VisibilityResult Trace(
        const VisibilityQuery& query) override;

private:
    [[nodiscard]] f64 SurfaceRadiusMeters(
        const math::Double3& bodyPoint,
        f64 footprintMeters) const noexcept;

    [[nodiscard]] f64 SignedHeightDistanceMeters(
        const math::Double3& bodyPoint,
        f64 footprintMeters) const noexcept;

    [[nodiscard]] math::Float3 SurfaceNormalInQueryFrame(
        const math::Double3& bodyPoint,
        const math::RigidTransformD& queryFromBody) const noexcept;

    universe::BodyId body_{};
    world::PlanetDefinition planet_{};
    const terrain::TerrainSource* source_{nullptr};
    const universe::BodyRegistry* bodies_{nullptr};
    const frames::FrameGraph* frames_{nullptr};
    TerrainVisibilityConfig config_{};
    time::SimulationTime atTime_{};
    VisibilityProviderDesc desc_{};
};
} // namespace orbit::lighting
