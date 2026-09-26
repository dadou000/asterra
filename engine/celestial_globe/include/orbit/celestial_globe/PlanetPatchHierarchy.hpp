#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <compare>
#include <cstddef>
#include <set>
#include <vector>

namespace orbit::celestial_globe
{
struct PlanetPatchId
{
    u8 face{0U};
    u8 level{0U};
    u32 x{0U};
    u32 y{0U};

    [[nodiscard]] auto operator<=>(
        const PlanetPatchId&) const noexcept = default;
};

struct PlanetPatchSelectorConfig
{
    // Every selected node renders a small regular grid. Keeping the grid
    // fixed makes the global hierarchy GPU friendly while the quadtree only
    // decides where those grids are distributed over the planet.
    u32 patchResolution{17U};
    u32 maximumLevel{12U};
    f64 targetCellPixels{1.5};
    f64 hysteresisFraction{0.18};
    u32 maximumSelectedPatches{4096U};
    bool horizonCulling{true};
    bool frustumCulling{true};
};

struct PlanetPatchView
{
    math::Double3 cameraPositionMeters{};
    math::Float3 cameraForward{0.0F, 0.0F, 1.0F};
    f64 verticalFovRadians{1.0471975511965976};
    u32 viewportWidthPixels{1U};
    u32 viewportHeightPixels{1U};

    // Conservative radial displacement bound used by culling. This is not a
    // rendering amplitude: it only expands patch bounds so mountains and
    // deep basins cannot be incorrectly discarded at the horizon/frustum.
    f64 maximumDisplacementMeters{0.0};
};

struct PlanetPatchSelectionStats
{
    u32 candidatesEvaluated{0U};
    u32 refinedNodes{0U};
    u32 horizonCulledNodes{0U};
    u32 frustumCulledNodes{0U};
    u32 selectedPatches{0U};
    u32 maximumSelectedLevel{0U};
    bool patchBudgetLimited{false};
};

struct PlanetPatchSelection
{
    std::vector<PlanetPatchId> patches;
    PlanetPatchSelectionStats stats{};
};

class PlanetPatchSelector
{
public:
    [[nodiscard]] PlanetPatchSelection Select(
        const universe::BodyShape& shape,
        const PlanetPatchView& view,
        const PlanetPatchSelectorConfig& config = {});

    void Reset() noexcept;

private:
    std::set<PlanetPatchId> previouslyRefined_;
};

struct PlanetPatchMeshConfig
{
    u32 patchResolution{17U};
    f64 footprintScale{1.5};

    // A radial skirt covers T-junction gaps where a fine patch meets a
    // coarser neighbour. Zero selects a footprint-derived depth.
    f64 skirtDepthMeters{0.0};
};

struct PlanetPatchVertex
{
    math::Double3 positionMeters{};
    math::Double3 normal{};
    f64 elevationMeters{0.0};
};

struct PlanetPatchMesh
{
    PlanetPatchId id{};
    std::vector<PlanetPatchVertex> vertices;
    std::vector<u32> indices;
    u32 surfaceVertexCount{0U};
    f64 referenceRadiusMeters{1.0};
    f64 minimumRadiusMeters{1.0};
    f64 maximumRadiusMeters{1.0};
    f64 sampleFootprintMeters{1.0};
    f64 skirtDepthMeters{0.0};
    u64 sourceRevision{0U};
    u64 fingerprint{0U};
};

[[nodiscard]] math::Double3 PlanetPatchDirection(
    PlanetPatchId id,
    f64 localU,
    f64 localV);

[[nodiscard]] u64 PlanetPatchFingerprint(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    PlanetPatchId id,
    const PlanetPatchMeshConfig& config = {});

[[nodiscard]] PlanetPatchMesh BuildPlanetPatch(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    PlanetPatchId id,
    const PlanetPatchMeshConfig& config = {});
} // namespace orbit::celestial_globe
