#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_erosion/SedimentExchange.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <array>
#include <span>
#include <vector>

namespace orbit::terrain_boundary
{
struct WaterBoundaryFlux
{
    f64 volumeCubicMeters{0.0};
    f64 dischargeCubicMetersPerSecond{0.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct MaterialGhostCell
{
    f32 surfaceHeightMeters{0.0F};
    f32 bedrockHeightMeters{0.0F};
    f32 regolithMeters{0.0F};
    f32 soilMeters{0.0F};
    f32 sandMeters{0.0F};
    f32 debrisMeters{0.0F};
    f32 moisture{0.0F};

    terrain_geology::RockTypeId bedrockMaterial{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct PhysicalBoundarySample
{
    MaterialGhostCell material{};
    terrain_hydrology::DrainageBoundaryCell drainage{};
    WaterBoundaryFlux water{};
    terrain_erosion::SedimentTransportPacket sediment{};

    [[nodiscard]] bool IsValid() const noexcept;

    [[nodiscard]] terrain_erosion::SedimentMass
    TotalSediment() const noexcept;

    [[nodiscard]] terrain_erosion::SedimentMass
    AeolianSediment() const noexcept;
};

struct PhysicalPageEdgeState
{
    terrain::PhysicalTerrainPageKey sourcePage{};
    world::TileEdge sourceEdge{world::TileEdge::North};

    std::vector<PhysicalBoundarySample> samples;

    // Upstream physical-process revision for this snapshot. View/cache
    // residency is deliberately absent.
    u64 revision{0};

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] f64 TotalWaterVolumeCubicMeters() const noexcept;
    [[nodiscard]] terrain_erosion::SedimentMass
    TotalSediment() const noexcept;
};

struct IncomingPhysicalPageEdge
{
    terrain::PhysicalTerrainPageKey destinationPage{};
    world::TileEdge receivingEdge{world::TileEdge::North};

    std::vector<PhysicalBoundarySample> samples;

    u64 revision{0};

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] f64 TotalWaterVolumeCubicMeters() const noexcept;
    [[nodiscard]] terrain_erosion::SedimentMass
    TotalSediment() const noexcept;
};

struct PhysicalPageHalo
{
    std::array<std::vector<PhysicalBoundarySample>, 4> edges{};
    std::array<u64, 4> revisions{};

    [[nodiscard]] bool IsComplete(u32 resolution) const noexcept;
    [[nodiscard]] const std::vector<PhysicalBoundarySample>&
    Edge(world::TileEdge edge) const;

    void SetEdge(const IncomingPhysicalPageEdge& incoming);
};

[[nodiscard]] PhysicalPageEdgeState BuildPhysicalPageEdgeState(
    const terrain::PhysicalTerrainPageKey& sourcePage,
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_hydrology::DrainagePage* drainage,
    world::TileEdge sourceEdge,
    std::span<const WaterBoundaryFlux> waterFlux = {},
    const terrain_erosion::SedimentBoundaryFlux* sedimentFlux = nullptr,
    u64 revision = 0);

[[nodiscard]] IncomingPhysicalPageEdge RemapPhysicalPageEdge(
    const PhysicalPageEdgeState& outgoing,
    const terrain::PhysicalTerrainPageKey& destinationPage);

[[nodiscard]] u64 BoundaryExchangeFingerprint(
    const terrain::PhysicalTerrainPageKey& sourcePage,
    const terrain::PhysicalTerrainPageKey& destinationPage,
    world::TileEdge sourceEdge,
    u64 sourceBoundaryRevision) noexcept;

// Imports the M14 portion of one incoming edge into the matching receiving
// edge cells while preserving M14 imported-mass accounting.
void ImportSedimentEdge(
    const IncomingPhysicalPageEdge& incoming,
    terrain_erosion::SedimentExchangePage& sediment);

// Utility for closed multi-page mass checks.
struct BoundaryTransportTotals
{
    f64 waterCubicMeters{0.0};
    terrain_erosion::SedimentMass sediment{};

    [[nodiscard]] bool IsValid() const noexcept;
};

[[nodiscard]] BoundaryTransportTotals TransportTotals(
    const PhysicalPageEdgeState& edge) noexcept;

[[nodiscard]] BoundaryTransportTotals TransportTotals(
    const IncomingPhysicalPageEdge& edge) noexcept;
} // namespace orbit::terrain_boundary
