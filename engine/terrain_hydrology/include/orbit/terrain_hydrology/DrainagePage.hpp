#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <array>
#include <span>
#include <vector>

namespace orbit::terrain_hydrology
{
// M09 depression policy. PreserveClosed keeps true local sinks. FillToBoundary
// conditions only the derived routing surface; the M08 material column is
// never modified.
enum class DepressionRoutingPolicy : u8
{
    PreserveClosed,
    FillToBoundary
};

struct DrainageRoutingConfig
{
    DepressionRoutingPolicy depressionPolicy{
        DepressionRoutingPolicy::FillToBoundary};

    // Strict drop used by the conditioned routing surface.
    f32 minimumDrainageDropMeters{0.01F};

    // M04 drainage guidance is a routing preference, not height authority.
    // It scales candidate downhill slopes but can never make an uphill
    // neighbor eligible. Keep below 1 so all guided scores remain positive.
    f32 authoredGuidanceWeight{0.20F};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct DrainageCellInput
{
    // Local runoff depth rate. Discharge is runoff * physical cell area and
    // accumulates downstream.
    f32 runoffMetersPerSecond{0.0F};

    // M04 authored drainage sample, conventionally normalized to [-1, 1].
    // Positive values attract downhill routing; negative values repel it.
    f32 authoredDrainage{0.0F};

    // Oceans and other explicitly-authored outlets remain fixed during
    // depression conditioning and terminate local flow.
    bool outlet{false};
};

// Fixed boundary state exchanged between adjacent physical pages. The
// conditioned elevation is imported from the neighboring M09 result, so a
// page edge is not silently treated as a local outlet. Area/discharge and
// flow direction carry cross-page flux back into the receiving page.
struct DrainageBoundaryCell
{
    f32 surfaceHeightMeters{0.0F};
    f32 conditionedHeightMeters{0.0F};
    f32 authoredDrainage{0.0F};

    f64 drainageAreaSquareMeters{0.0};
    f64 dischargeCubicMetersPerSecond{0.0};

    i8 flowDx{0};
    i8 flowDy{0};

    [[nodiscard]] bool IsValid() const noexcept;
};

enum class DrainageBoundarySide : u8
{
    North,
    East,
    South,
    West
};

struct DrainagePageHalo
{
    // Entries use the local orientation of the receiving page. Cross-cube-face
    // remapping therefore belongs to the M01/world-neighborhood layer.
    std::vector<DrainageBoundaryCell> north;
    std::vector<DrainageBoundaryCell> east;
    std::vector<DrainageBoundaryCell> south;
    std::vector<DrainageBoundaryCell> west;

    // NW, NE, SE, SW in the receiving page's local orientation.
    std::array<DrainageBoundaryCell, 4> corners{};

    // Boundary-state revision supplied by the page-neighborhood scheduler.
    // Camera/cache residency never participates in this value.
    u64 revision{0};

    [[nodiscard]] bool IsComplete(u32 resolution) const noexcept;
};

struct DrainageFlowTarget
{
    i8 dx{0};
    i8 dy{0};
    bool exitsPage{false};

    [[nodiscard]] bool HasDownstream() const noexcept
    {
        return dx != 0 || dy != 0;
    }
};

struct DrainageCell
{
    f32 surfaceHeightMeters{0.0F};
    f32 drainageElevationMeters{0.0F};
    f32 depressionFillMeters{0.0F};
    f32 authoredDrainage{0.0F};

    f64 drainageAreaSquareMeters{0.0};
    f64 dischargeCubicMetersPerSecond{0.0};

    DrainageFlowTarget flow{};
    bool outlet{false};
};

class DrainagePage
{
public:
    [[nodiscard]] u32 Resolution() const noexcept;
    [[nodiscard]] f64 SpacingMeters() const noexcept;
    [[nodiscard]] u64 Revision() const noexcept;
    [[nodiscard]] const terrain::PhysicalTerrainPageKey&
    SourcePage() const noexcept;

    [[nodiscard]] DrainageCell& At(u32 x, u32 y);
    [[nodiscard]] const DrainageCell& At(u32 x, u32 y) const;

    // Exports one edge cell in the local page orientation. For same-face
    // neighbors the flow vector can be copied directly into the opposite halo.
    // Cross-face neighbors remap it through the canonical M01 neighborhood.
    [[nodiscard]] DrainageBoundaryCell BoundaryCell(
        DrainageBoundarySide side,
        u32 index) const;

private:
    friend DrainagePage BuildDrainagePage(
        const terrain_material_column::MaterialColumnPage&,
        const terrain::PhysicalTerrainPageKey&,
        std::span<const DrainageCellInput>,
        const DrainagePageHalo&,
        const DrainageRoutingConfig&);

    terrain::PhysicalTerrainPageKey sourcePage_{};
    DrainageRoutingConfig config_{};
    u64 revision_{0};
    u32 resolution_{0};
    f64 spacingMeters_{0.0};
    std::vector<DrainageCell> cells_;
};

// Deterministic M09 invalidation identity. It is derived from the M08 physical
// page identity, routing settings and exchanged boundary revision only.
[[nodiscard]] u64 DrainageRevisionFingerprint(
    const terrain::PhysicalTerrainPageKey& sourcePage,
    const DrainageRoutingConfig& config,
    u64 haloRevision) noexcept;

// Builds deterministic D8 flow, depression conditioning, drainage area and
// discharge directly from the M08 physical material-column surface.
//
// FillToBoundary requires a complete one-cell halo whose conditioned heights
// are fixed boundary conditions from neighboring M09 pages. This is the seam
// contract: a page boundary is never invented as an outlet simply because the
// page was generated independently.
[[nodiscard]] DrainagePage BuildDrainagePage(
    const terrain_material_column::MaterialColumnPage& materialColumn,
    const terrain::PhysicalTerrainPageKey& sourcePage,
    std::span<const DrainageCellInput> inputs,
    const DrainagePageHalo& halo,
    const DrainageRoutingConfig& config = {});

} // namespace orbit::terrain_hydrology
