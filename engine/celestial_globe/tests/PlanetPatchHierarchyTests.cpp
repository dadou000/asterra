#include <orbit/celestial_globe/PlanetPatchHierarchy.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace
{
class TestTerrain final : public orbit::terrain::TerrainSource
{
public:
    explicit TestTerrain(const orbit::u64 revision)
        : revision_(revision)
    {
    }

    orbit::terrain::TerrainSample Sample(
        const orbit::terrain::TerrainQuery& query) const noexcept override
    {
        const auto& d = query.unitDirection;
        const double broad =
            1800.0 * d.y +
            700.0 * d.x * d.z;
        return {
            .elevationMeters = broad,
            .coarseElevationMeters = broad
        };
    }

    orbit::u64 Revision() const noexcept override
    {
        return revision_;
    }

private:
    orbit::u64 revision_{0U};
};

[[nodiscard]] bool Near(
    const orbit::math::Double3 a,
    const orbit::math::Double3 b,
    const double epsilon)
{
    return orbit::math::Length(a - b) <= epsilon;
}
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::celestial_globe;

    const universe::BodyShape shape =
        universe::SphereShape{.radiusMeters = 1.0e6};
    TestTerrain terrain(71U);

    // Sibling patch parameterizations must share exactly the same spherical
    // boundary. Otherwise refinement creates cracks before displacement is
    // even sampled.
    const PlanetPatchId left{0U, 1U, 0U, 0U};
    const PlanetPatchId right{0U, 1U, 1U, 0U};
    for (u32 step = 0U; step <= 8U; ++step)
    {
        const f64 v = static_cast<f64>(step) / 8.0;
        if (!Near(
                PlanetPatchDirection(left, 1.0, v),
                PlanetPatchDirection(right, 0.0, v),
                1.0e-12))
        {
            return 1;
        }
    }

    const PlanetPatchMeshConfig meshConfig{
        .patchResolution = 17U,
        .footprintScale = 1.5,
        .skirtDepthMeters = 0.0
    };

    const auto leftMesh = BuildPlanetPatch(
        terrain,
        shape,
        left,
        meshConfig);
    const auto rightMesh = BuildPlanetPatch(
        terrain,
        shape,
        right,
        meshConfig);

    if (leftMesh.surfaceVertexCount != 17U * 17U ||
        leftMesh.vertices.size() <= leftMesh.surfaceVertexCount ||
        leftMesh.indices.size() <= 16U * 16U * 6U ||
        leftMesh.sampleFootprintMeters <= 0.0 ||
        leftMesh.skirtDepthMeters <= 0.0)
    {
        return 2;
    }

    // Terrain sampling is deterministic on shared sibling borders as well.
    for (u32 y = 0U; y < 17U; ++y)
    {
        const auto& a = leftMesh.vertices[y * 17U + 16U].positionMeters;
        const auto& b = rightMesh.vertices[y * 17U].positionMeters;
        if (!Near(a, b, 1.0e-6))
        {
            return 3;
        }
    }

    for (u32 index = 0U; index < leftMesh.surfaceVertexCount; ++index)
    {
        const f64 normalLength = math::Length(leftMesh.vertices[index].normal);
        if (!std::isfinite(normalLength) ||
            std::abs(normalLength - 1.0) > 1.0e-8)
        {
            return 4;
        }
    }

    TestTerrain revisedTerrain(72U);
    if (PlanetPatchFingerprint(terrain, shape, left, meshConfig) ==
            PlanetPatchFingerprint(revisedTerrain, shape, left, meshConfig) ||
        PlanetPatchFingerprint(terrain, shape, left, meshConfig) ==
            PlanetPatchFingerprint(terrain, shape, right, meshConfig))
    {
        return 5;
    }

    PlanetPatchSelector selector;
    PlanetPatchSelectorConfig selectorConfig{
        .patchResolution = 17U,
        .maximumLevel = 8U,
        .targetCellPixels = 2.0,
        .hysteresisFraction = 0.20,
        .maximumSelectedPatches = 1024U,
        .horizonCulling = true,
        .frustumCulling = true
    };

    const PlanetPatchView farView{
        .cameraPositionMeters = {0.0, 0.0, 8.0e6},
        .cameraForward = {0.0F, 0.0F, -1.0F},
        .verticalFovRadians = 1.0,
        .viewportWidthPixels = 1920U,
        .viewportHeightPixels = 1080U,
        .maximumDisplacementMeters = 3000.0
    };

    const auto farSelection = selector.Select(shape, farView, selectorConfig);
    if (farSelection.patches.empty() ||
        farSelection.patches.size() > selectorConfig.maximumSelectedPatches ||
        farSelection.stats.horizonCulledNodes == 0U)
    {
        return 6;
    }

    const PlanetPatchView nearView{
        .cameraPositionMeters = {0.0, 0.0, 1.08e6},
        .cameraForward = {0.0F, 0.0F, -1.0F},
        .verticalFovRadians = 1.0,
        .viewportWidthPixels = 1920U,
        .viewportHeightPixels = 1080U,
        .maximumDisplacementMeters = 3000.0
    };

    const auto nearSelection = selector.Select(shape, nearView, selectorConfig);
    if (nearSelection.patches.empty() ||
        nearSelection.stats.maximumSelectedLevel <=
            farSelection.stats.maximumSelectedLevel ||
        nearSelection.patches.size() <= farSelection.patches.size())
    {
        return 7;
    }

    const auto repeatedNear = selector.Select(shape, nearView, selectorConfig);
    if (nearSelection.patches != repeatedNear.patches)
    {
        return 8;
    }

    const std::set<PlanetPatchId> uniqueNear(
        nearSelection.patches.begin(),
        nearSelection.patches.end());
    if (uniqueNear.size() != nearSelection.patches.size())
    {
        return 9;
    }

    selector.Reset();
    auto budgetConfig = selectorConfig;
    budgetConfig.maximumSelectedPatches = 24U;
    budgetConfig.targetCellPixels = 0.05;
    budgetConfig.maximumLevel = 12U;

    const auto budgetSelection = selector.Select(shape, nearView, budgetConfig);
    if (!budgetSelection.stats.patchBudgetLimited ||
        budgetSelection.patches.size() > budgetConfig.maximumSelectedPatches)
    {
        return 10;
    }

    // A side-looking camera should reject nodes before subdivision instead of
    // wasting the orbital LOD budget behind the viewer.
    selector.Reset();
    auto sideView = farView;
    sideView.cameraForward = {1.0F, 0.0F, 0.0F};
    const auto sideSelection = selector.Select(shape, sideView, selectorConfig);
    if (sideSelection.stats.frustumCulledNodes == 0U)
    {
        return 11;
    }

    return 0;
}
