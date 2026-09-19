#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_erosion/SedimentExchange.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/terrain_water/CoastalProcess.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace orbit::terrain_region
{
// Conservative water packet for one physical boundary sample.
// velocityMoment = transported volume * local tangent velocity.
// Keeping the conserved moment separate from velocity makes merges exact.
struct WaterBoundaryFluxPacket
{
    f64 volumeCubicMeters{0.0};
    math::Double2 velocityMoment{};

    [[nodiscard]] bool IsValid() const noexcept;

    WaterBoundaryFluxPacket& operator+=(
        const WaterBoundaryFluxPacket& other) noexcept;
};

[[nodiscard]] WaterBoundaryFluxPacket operator+(
    WaterBoundaryFluxPacket left,
    const WaterBoundaryFluxPacket& right) noexcept;

struct SurfaceBoundaryFlux
{
    u32 resolution{0};
    u64 revision{0};

    std::vector<WaterBoundaryFluxPacket> north;
    std::vector<WaterBoundaryFluxPacket> east;
    std::vector<WaterBoundaryFluxPacket> south;
    std::vector<WaterBoundaryFluxPacket> west;

    // NW, NE, SE, SW in the source page's local orientation.
    std::array<WaterBoundaryFluxPacket, 4> corners{};

    // M14 canonical typed mobile sediment. Airborne and surface-mobile lanes
    // are the M25 aeolian boundary flux; waterborne is the hydraulic/coastal
    // sediment flux. No duplicate sediment authority is introduced.
    terrain_erosion::SedimentBoundaryFlux sediment{};

    [[nodiscard]] bool IsComplete() const noexcept;
    [[nodiscard]] f64 TotalWaterVolumeCubicMeters() const noexcept;
    [[nodiscard]] terrain_erosion::SedimentMass
    TotalSediment() const noexcept;
};

[[nodiscard]] SurfaceBoundaryFlux MakeSurfaceBoundaryFlux(
    u32 resolution,
    u64 revision,
    terrain_erosion::SedimentBoundaryFlux sediment = {});

struct PhysicalPageBoundaryFlux
{
    terrain::PhysicalTerrainPageAddress address{};
    SurfaceBoundaryFlux outgoing{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct SurfaceBoundaryEdgeTransfer
{
    terrain::PhysicalTerrainPageAddress source{};
    terrain::PhysicalTerrainPageAddress target{};

    world::TileEdge sourceEdge{world::TileEdge::North};
    world::TileEdge targetEdge{world::TileEdge::North};
    bool reversed{false};

    u64 revision{0};

    // Already expressed in target-local edge order/orientation.
    std::vector<WaterBoundaryFluxPacket> water;
    std::vector<terrain_erosion::SedimentTransportPacket> sediment;
};

enum class SurfaceBoundaryCorner : u8
{
    NorthWest = 0,
    NorthEast = 1,
    SouthEast = 2,
    SouthWest = 3
};

struct SurfaceBoundaryCornerTransfer
{
    terrain::PhysicalTerrainPageAddress source{};
    terrain::PhysicalTerrainPageAddress target{};

    SurfaceBoundaryCorner sourceCorner{
        SurfaceBoundaryCorner::NorthWest};
    SurfaceBoundaryCorner targetCorner{
        SurfaceBoundaryCorner::NorthWest};

    u64 revision{0};

    WaterBoundaryFluxPacket water{};
    terrain_erosion::SedimentTransportPacket sediment{};
};

struct SurfaceBoundaryTransferBatch
{
    std::vector<SurfaceBoundaryEdgeTransfer> edges;
    std::vector<SurfaceBoundaryCornerTransfer> corners;

    [[nodiscard]] f64 TotalWaterVolumeCubicMeters() const noexcept;
    [[nodiscard]] terrain_erosion::SedimentMass
    TotalSediment() const noexcept;
};

// Builds every cardinal and diagonal transfer in deterministic physical-address
// order. Input order never changes result ordering or sample mapping.
[[nodiscard]] SurfaceBoundaryTransferBatch
BuildDeterministicBoundaryTransfers(
    std::span<const PhysicalPageBoundaryFlux> pages);

// Imports every matching M14 packet into the receiver. Missing neighbors are
// treated as zero incoming mass. This preserves M14 accounting.
void ApplySedimentBoundaryTransfers(
    const terrain::PhysicalTerrainPageAddress& receiver,
    const SurfaceBoundaryTransferBatch& transfers,
    terrain_erosion::SedimentExchangePage& sediment);

// Water remains solver-owned. This returns the exact conservative incoming
// packet total for a receiver so hydraulic/coastal solvers can consume it.
[[nodiscard]] WaterBoundaryFluxPacket
IncomingWaterBoundaryFlux(
    const terrain::PhysicalTerrainPageAddress& receiver,
    const SurfaceBoundaryTransferBatch& transfers) noexcept;

// Transform a local page-grid/tangent vector across one mapped tile edge.
// +x = increasing tile u, +y = increasing tile v (south).
[[nodiscard]] math::Double2 TransformBoundaryVectorAcrossEdge(
    world::TileEdge sourceEdge,
    const world::TileEdgeNeighborMapping& mapping,
    math::Double2 sourceVector) noexcept;

struct SurfaceGhostCell
{
    terrain_material_column::MaterialColumnCell material{};

    std::optional<terrain_hydrology::DrainageBoundaryCell> drainage;
    std::optional<terrain_water::CoastalBoundaryCell> water;

    [[nodiscard]] bool IsValid() const noexcept;
};

struct SurfaceBoundarySnapshot
{
    terrain::PhysicalTerrainPageAddress address{};
    u32 resolution{0};
    f64 spacingMeters{0.0};
    u64 revision{0};

    std::vector<SurfaceGhostCell> north;
    std::vector<SurfaceGhostCell> east;
    std::vector<SurfaceGhostCell> south;
    std::vector<SurfaceGhostCell> west;

    // NW, NE, SE, SW.
    std::array<SurfaceGhostCell, 4> corners{};

    [[nodiscard]] bool IsComplete() const noexcept;
};

// Captures one-cell physical ghost state from M08 plus optional M09/M17 state.
// The snapshot is physical derived state; no camera/cache identity participates.
[[nodiscard]] SurfaceBoundarySnapshot BuildSurfaceBoundarySnapshot(
    const terrain::PhysicalTerrainPageAddress& address,
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_hydrology::DrainagePage* drainage = nullptr,
    const terrain_water::CoastalWaterPage* water = nullptr,
    u64 revision = 0);

struct SurfaceGhostEdgeTransfer
{
    terrain::PhysicalTerrainPageAddress source{};
    terrain::PhysicalTerrainPageAddress target{};

    world::TileEdge sourceEdge{world::TileEdge::North};
    world::TileEdge targetEdge{world::TileEdge::North};
    bool reversed{false};

    u64 revision{0};
    std::vector<SurfaceGhostCell> cells;
};

struct SurfaceGhostCornerTransfer
{
    terrain::PhysicalTerrainPageAddress source{};
    terrain::PhysicalTerrainPageAddress target{};

    SurfaceBoundaryCorner sourceCorner{
        SurfaceBoundaryCorner::NorthWest};
    SurfaceBoundaryCorner targetCorner{
        SurfaceBoundaryCorner::NorthWest};

    u64 revision{0};
    SurfaceGhostCell cell{};
};

struct SurfaceGhostTransferBatch
{
    std::vector<SurfaceGhostEdgeTransfer> edges;
    std::vector<SurfaceGhostCornerTransfer> corners;
};

[[nodiscard]] SurfaceGhostTransferBatch
BuildDeterministicGhostTransfers(
    std::span<const SurfaceBoundarySnapshot> pages);

// Assemble the exact M09 one-cell halo for one target page from M25 ghost
// transfers. Throws if any cardinal/corner drainage state is unavailable.
[[nodiscard]] terrain_hydrology::DrainagePageHalo
BuildDrainageHaloFromGhostTransfers(
    const terrain::PhysicalTerrainPageAddress& receiver,
    u32 resolution,
    const SurfaceGhostTransferBatch& transfers);

// Assemble M17 neighbor water boundary state. The four cardinal neighbors must
// be present and carry coastal state; corners are not used by the finite-volume
// shallow-water boundary.
[[nodiscard]] terrain_water::CoastalBoundaryState
BuildCoastalBoundaryFromGhostTransfers(
    const terrain::PhysicalTerrainPageAddress& receiver,
    u32 resolution,
    const SurfaceGhostTransferBatch& transfers);
} // namespace orbit::terrain_region
