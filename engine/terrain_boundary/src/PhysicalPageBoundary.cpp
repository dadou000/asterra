#include <orbit/terrain_boundary/PhysicalPageBoundary.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::terrain_boundary
{
namespace
{
[[nodiscard]] constexpr std::size_t EdgeIndex(
    const world::TileEdge edge) noexcept
{
    return static_cast<std::size_t>(edge);
}

[[nodiscard]] bool FiniteNonNegative(const f64 value) noexcept
{
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] const terrain_erosion::SedimentTransportPacket&
PacketAt(
    const terrain_erosion::SedimentBoundaryFlux& flux,
    const world::TileEdge edge,
    const u32 index)
{
    switch (edge)
    {
    case world::TileEdge::North:
        return flux.north.at(index);
    case world::TileEdge::East:
        return flux.east.at(index);
    case world::TileEdge::South:
        return flux.south.at(index);
    case world::TileEdge::West:
        return flux.west.at(index);
    }

    return flux.north.at(index);
}

[[nodiscard]] std::pair<u32, u32> BoundaryCoordinate(
    const world::TileEdge edge,
    const u32 index,
    const u32 resolution) noexcept
{
    switch (edge)
    {
    case world::TileEdge::North:
        return {index, 0U};
    case world::TileEdge::East:
        return {resolution - 1U, index};
    case world::TileEdge::South:
        return {index, resolution - 1U};
    case world::TileEdge::West:
        return {0U, index};
    }

    return {index, 0U};
}

[[nodiscard]] terrain_hydrology::DrainageBoundarySide
ToDrainageSide(const world::TileEdge edge) noexcept
{
    switch (edge)
    {
    case world::TileEdge::North:
        return terrain_hydrology::DrainageBoundarySide::North;
    case world::TileEdge::East:
        return terrain_hydrology::DrainageBoundarySide::East;
    case world::TileEdge::South:
        return terrain_hydrology::DrainageBoundarySide::South;
    case world::TileEdge::West:
        return terrain_hydrology::DrainageBoundarySide::West;
    }

    return terrain_hydrology::DrainageBoundarySide::North;
}

[[nodiscard]] MaterialGhostCell BuildGhost(
    const terrain_material_column::MaterialColumnCell& cell) noexcept
{
    return {
        .surfaceHeightMeters = cell.SurfaceHeightMeters(),
        .bedrockHeightMeters = cell.bedrockHeightMeters,
        .regolithMeters = cell.regolithMeters,
        .soilMeters = cell.soilMeters,
        .sandMeters = cell.sandMeters,
        .debrisMeters = cell.debrisMeters,
        .moisture = cell.moisture,
        .bedrockMaterial = cell.bedrockMaterial
    };
}

void SetFluxEdge(
    terrain_erosion::SedimentBoundaryFlux& flux,
    const world::TileEdge edge,
    std::vector<terrain_erosion::SedimentTransportPacket> packets)
{
    switch (edge)
    {
    case world::TileEdge::North:
        flux.north = std::move(packets);
        break;
    case world::TileEdge::East:
        flux.east = std::move(packets);
        break;
    case world::TileEdge::South:
        flux.south = std::move(packets);
        break;
    case world::TileEdge::West:
        flux.west = std::move(packets);
        break;
    }
}
} // namespace

bool WaterBoundaryFlux::IsValid() const noexcept
{
    return
        FiniteNonNegative(volumeCubicMeters) &&
        std::isfinite(dischargeCubicMetersPerSecond);
}

bool MaterialGhostCell::IsValid() const noexcept
{
    return
        std::isfinite(surfaceHeightMeters) &&
        std::isfinite(bedrockHeightMeters) &&
        FiniteNonNegative(regolithMeters) &&
        FiniteNonNegative(soilMeters) &&
        FiniteNonNegative(sandMeters) &&
        FiniteNonNegative(debrisMeters) &&
        std::isfinite(moisture) &&
        moisture >= 0.0F &&
        moisture <= 1.0F &&
        bedrockMaterial.IsValid();
}

bool PhysicalBoundarySample::IsValid() const noexcept
{
    return
        material.IsValid() &&
        drainage.IsValid() &&
        water.IsValid() &&
        sediment.waterborne.IsValid() &&
        sediment.airborne.IsValid() &&
        sediment.surfaceMobile.IsValid();
}

terrain_erosion::SedimentMass
PhysicalBoundarySample::TotalSediment() const noexcept
{
    return sediment.Total();
}

terrain_erosion::SedimentMass
PhysicalBoundarySample::AeolianSediment() const noexcept
{
    return sediment.airborne;
}

bool PhysicalPageEdgeState::IsValid() const noexcept
{
    if (!sourcePage.address.planet.IsValid() ||
        sourcePage.resolution == 0 ||
        samples.size() !=
            static_cast<std::size_t>(sourcePage.resolution))
    {
        return false;
    }

    return std::all_of(
        samples.begin(),
        samples.end(),
        [](const PhysicalBoundarySample& sample)
        {
            return sample.IsValid();
        });
}

f64 PhysicalPageEdgeState::TotalWaterVolumeCubicMeters() const noexcept
{
    f64 total = 0.0;
    for (const auto& sample : samples)
    {
        total += sample.water.volumeCubicMeters;
    }
    return total;
}

terrain_erosion::SedimentMass
PhysicalPageEdgeState::TotalSediment() const noexcept
{
    terrain_erosion::SedimentMass total{};
    for (const auto& sample : samples)
    {
        total += sample.TotalSediment();
    }
    return total;
}

bool IncomingPhysicalPageEdge::IsValid() const noexcept
{
    if (!destinationPage.address.planet.IsValid() ||
        destinationPage.resolution == 0 ||
        samples.size() !=
            static_cast<std::size_t>(destinationPage.resolution))
    {
        return false;
    }

    return std::all_of(
        samples.begin(),
        samples.end(),
        [](const PhysicalBoundarySample& sample)
        {
            return sample.IsValid();
        });
}

f64 IncomingPhysicalPageEdge::TotalWaterVolumeCubicMeters() const noexcept
{
    f64 total = 0.0;
    for (const auto& sample : samples)
    {
        total += sample.water.volumeCubicMeters;
    }
    return total;
}

terrain_erosion::SedimentMass
IncomingPhysicalPageEdge::TotalSediment() const noexcept
{
    terrain_erosion::SedimentMass total{};
    for (const auto& sample : samples)
    {
        total += sample.TotalSediment();
    }
    return total;
}

bool PhysicalPageHalo::IsComplete(const u32 resolution) const noexcept
{
    if (resolution == 0)
    {
        return false;
    }

    for (const auto& edge : edges)
    {
        if (edge.size() !=
            static_cast<std::size_t>(resolution))
        {
            return false;
        }

        if (!std::all_of(
                edge.begin(),
                edge.end(),
                [](const PhysicalBoundarySample& sample)
                {
                    return sample.IsValid();
                }))
        {
            return false;
        }
    }

    return true;
}

const std::vector<PhysicalBoundarySample>&
PhysicalPageHalo::Edge(const world::TileEdge edge) const
{
    return edges.at(EdgeIndex(edge));
}

void PhysicalPageHalo::SetEdge(
    const IncomingPhysicalPageEdge& incoming)
{
    if (!incoming.IsValid())
    {
        throw std::invalid_argument(
            "M25 incoming physical page edge is invalid.");
    }

    const std::size_t index =
        EdgeIndex(incoming.receivingEdge);

    edges.at(index) = incoming.samples;
    revisions.at(index) = incoming.revision;
}

PhysicalPageEdgeState BuildPhysicalPageEdgeState(
    const terrain::PhysicalTerrainPageKey& sourcePage,
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_hydrology::DrainagePage* drainage,
    const world::TileEdge sourceEdge,
    const std::span<const WaterBoundaryFlux> waterFlux,
    const terrain_erosion::SedimentBoundaryFlux* sedimentFlux,
    const u64 revision)
{
    if (!sourcePage.address.planet.IsValid() ||
        sourcePage.resolution == 0 ||
        material.Resolution() != sourcePage.resolution)
    {
        throw std::invalid_argument(
            "M25 source physical page/material dimensions are invalid.");
    }

    if (drainage != nullptr &&
        (drainage->Resolution() != sourcePage.resolution ||
         drainage->SourcePage().address != sourcePage.address))
    {
        throw std::invalid_argument(
            "M25 drainage page does not match the source physical page.");
    }

    if (!waterFlux.empty() &&
        waterFlux.size() !=
            static_cast<std::size_t>(sourcePage.resolution))
    {
        throw std::invalid_argument(
            "M25 water boundary flux must match page resolution.");
    }

    if (sedimentFlux != nullptr &&
        !sedimentFlux->IsComplete(sourcePage.resolution))
    {
        throw std::invalid_argument(
            "M25 sediment boundary flux must be complete.");
    }

    PhysicalPageEdgeState result{
        .sourcePage = sourcePage,
        .sourceEdge = sourceEdge,
        .samples =
            std::vector<PhysicalBoundarySample>(
                sourcePage.resolution),
        .revision = revision
    };

    for (u32 index = 0;
         index < sourcePage.resolution;
         ++index)
    {
        const auto [x, y] =
            BoundaryCoordinate(
                sourceEdge,
                index,
                sourcePage.resolution);

        auto& sample = result.samples[index];
        sample.material = BuildGhost(material.At(x, y));

        if (drainage != nullptr)
        {
            sample.drainage =
                drainage->BoundaryCell(
                    ToDrainageSide(sourceEdge),
                    index);
        }
        else
        {
            sample.drainage = {
                .surfaceHeightMeters =
                    sample.material.surfaceHeightMeters,
                .conditionedHeightMeters =
                    sample.material.surfaceHeightMeters,
                .authoredDrainage = 0.0F,
                .drainageAreaSquareMeters = 0.0,
                .dischargeCubicMetersPerSecond = 0.0,
                .flowDx = 0,
                .flowDy = 0
            };
        }

        if (!waterFlux.empty())
        {
            sample.water = waterFlux[index];
        }

        if (sedimentFlux != nullptr)
        {
            sample.sediment =
                PacketAt(
                    *sedimentFlux,
                    sourceEdge,
                    index);
        }

        if (!sample.IsValid())
        {
            throw std::invalid_argument(
                "M25 source boundary contains invalid physical state.");
        }
    }

    return result;
}

IncomingPhysicalPageEdge RemapPhysicalPageEdge(
    const PhysicalPageEdgeState& outgoing,
    const terrain::PhysicalTerrainPageKey& destinationPage)
{
    if (!outgoing.IsValid() ||
        destinationPage.resolution !=
            outgoing.sourcePage.resolution ||
        destinationPage.address.planet !=
            outgoing.sourcePage.address.planet)
    {
        throw std::invalid_argument(
            "M25 boundary remap page identity/resolution mismatch.");
    }

    const auto mapping =
        world::NeighborAcrossTileEdge(
            outgoing.sourcePage.address.tile,
            outgoing.sourceEdge);

    if (mapping.tile != destinationPage.address.tile)
    {
        throw std::invalid_argument(
            "M25 destination is not the physical neighbor for this edge.");
    }

    IncomingPhysicalPageEdge incoming{
        .destinationPage = destinationPage,
        .receivingEdge = mapping.edge,
        .samples =
            std::vector<PhysicalBoundarySample>(
                destinationPage.resolution),
        .revision =
            BoundaryExchangeFingerprint(
                outgoing.sourcePage,
                destinationPage,
                outgoing.sourceEdge,
                outgoing.revision)
    };

    for (u32 sourceIndex = 0;
         sourceIndex < outgoing.sourcePage.resolution;
         ++sourceIndex)
    {
        const u32 destinationIndex =
            world::RemapTileEdgeSampleIndex(
                mapping,
                sourceIndex,
                outgoing.sourcePage.resolution);

        PhysicalBoundarySample sample =
            outgoing.samples[sourceIndex];

        const auto transformed =
            world::TransformFlowAcrossTileEdge(
                outgoing.sourceEdge,
                mapping,
                {
                    .dx = sample.drainage.flowDx,
                    .dy = sample.drainage.flowDy
                });

        sample.drainage.flowDx = transformed.dx;
        sample.drainage.flowDy = transformed.dy;

        incoming.samples[destinationIndex] =
            std::move(sample);
    }

    if (!incoming.IsValid())
    {
        throw std::runtime_error(
            "M25 remapped physical boundary became invalid.");
    }

    return incoming;
}

u64 BoundaryExchangeFingerprint(
    const terrain::PhysicalTerrainPageKey& sourcePage,
    const terrain::PhysicalTerrainPageKey& destinationPage,
    const world::TileEdge sourceEdge,
    const u64 sourceBoundaryRevision) noexcept
{
    u64 value = terrain::StableCombine64(
        0x4D3235424E445259ULL, // "M25BNDRY"
        terrain::PhysicalPageFingerprint(sourcePage));

    value = terrain::StableCombine64(
        value,
        terrain::PhysicalPageFingerprint(destinationPage));

    value = terrain::StableCombine64(
        value,
        static_cast<u64>(sourceEdge));

    value = terrain::StableCombine64(
        value,
        sourceBoundaryRevision);

    return value;
}

void ImportSedimentEdge(
    const IncomingPhysicalPageEdge& incoming,
    terrain_erosion::SedimentExchangePage& sediment)
{
    if (!incoming.IsValid() ||
        sediment.Resolution() !=
            incoming.destinationPage.resolution)
    {
        throw std::invalid_argument(
            "M25 incoming sediment edge does not match destination page.");
    }

    const u32 resolution =
        incoming.destinationPage.resolution;

    terrain_erosion::SedimentBoundaryFlux flux{
        .north =
            std::vector<
                terrain_erosion::SedimentTransportPacket>(
                    resolution),
        .east =
            std::vector<
                terrain_erosion::SedimentTransportPacket>(
                    resolution),
        .south =
            std::vector<
                terrain_erosion::SedimentTransportPacket>(
                    resolution),
        .west =
            std::vector<
                terrain_erosion::SedimentTransportPacket>(
                    resolution),
        .revision = incoming.revision
    };

    std::vector<terrain_erosion::SedimentTransportPacket>
        packets(resolution);

    for (u32 index = 0; index < resolution; ++index)
    {
        packets[index] =
            incoming.samples[index].sediment;
    }

    SetFluxEdge(
        flux,
        incoming.receivingEdge,
        std::move(packets));

    sediment.ImportBoundaryFlux(flux);
}

bool BoundaryTransportTotals::IsValid() const noexcept
{
    return
        FiniteNonNegative(waterCubicMeters) &&
        sediment.IsValid();
}

BoundaryTransportTotals TransportTotals(
    const PhysicalPageEdgeState& edge) noexcept
{
    return {
        .waterCubicMeters =
            edge.TotalWaterVolumeCubicMeters(),
        .sediment = edge.TotalSediment()
    };
}

BoundaryTransportTotals TransportTotals(
    const IncomingPhysicalPageEdge& edge) noexcept
{
    return {
        .waterCubicMeters =
            edge.TotalWaterVolumeCubicMeters(),
        .sediment = edge.TotalSediment()
    };
}
} // namespace orbit::terrain_boundary
