#include <orbit/terrain_region/SurfaceBoundaryExchange.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace orbit::terrain_region
{
namespace
{
[[nodiscard]] bool Finite(const f64 value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] bool SameAddress(
    const terrain::PhysicalTerrainPageAddress& a,
    const terrain::PhysicalTerrainPageAddress& b) noexcept
{
    return a == b;
}

[[nodiscard]] auto AddressTuple(
    const terrain::PhysicalTerrainPageAddress& address) noexcept
{
    return std::tuple{
        address.planet.high,
        address.planet.low,
        static_cast<u8>(address.tile.face),
        address.tile.level,
        address.tile.x,
        address.tile.y};
}

[[nodiscard]] bool AddressLess(
    const terrain::PhysicalTerrainPageAddress& a,
    const terrain::PhysicalTerrainPageAddress& b) noexcept
{
    return AddressTuple(a) < AddressTuple(b);
}

[[nodiscard]] std::vector<WaterBoundaryFluxPacket>&
WaterEdge(
    SurfaceBoundaryFlux& flux,
    const world::TileEdge edge) noexcept
{
    switch (edge)
    {
    case world::TileEdge::North:
        return flux.north;
    case world::TileEdge::East:
        return flux.east;
    case world::TileEdge::South:
        return flux.south;
    case world::TileEdge::West:
        return flux.west;
    }

    return flux.north;
}

[[nodiscard]] const std::vector<WaterBoundaryFluxPacket>&
WaterEdge(
    const SurfaceBoundaryFlux& flux,
    const world::TileEdge edge) noexcept
{
    return WaterEdge(
        const_cast<SurfaceBoundaryFlux&>(flux),
        edge);
}

[[nodiscard]] std::vector<
    terrain_erosion::SedimentTransportPacket>&
SedimentEdge(
    terrain_erosion::SedimentBoundaryFlux& flux,
    const world::TileEdge edge) noexcept
{
    switch (edge)
    {
    case world::TileEdge::North:
        return flux.north;
    case world::TileEdge::East:
        return flux.east;
    case world::TileEdge::South:
        return flux.south;
    case world::TileEdge::West:
        return flux.west;
    }

    return flux.north;
}

[[nodiscard]] const std::vector<
    terrain_erosion::SedimentTransportPacket>&
SedimentEdge(
    const terrain_erosion::SedimentBoundaryFlux& flux,
    const world::TileEdge edge) noexcept
{
    return SedimentEdge(
        const_cast<terrain_erosion::SedimentBoundaryFlux&>(flux),
        edge);
}

[[nodiscard]] std::vector<SurfaceGhostCell>&
GhostEdge(
    SurfaceBoundarySnapshot& snapshot,
    const world::TileEdge edge) noexcept
{
    switch (edge)
    {
    case world::TileEdge::North:
        return snapshot.north;
    case world::TileEdge::East:
        return snapshot.east;
    case world::TileEdge::South:
        return snapshot.south;
    case world::TileEdge::West:
        return snapshot.west;
    }

    return snapshot.north;
}

[[nodiscard]] const std::vector<SurfaceGhostCell>&
GhostEdge(
    const SurfaceBoundarySnapshot& snapshot,
    const world::TileEdge edge) noexcept
{
    return GhostEdge(
        const_cast<SurfaceBoundarySnapshot&>(snapshot),
        edge);
}

[[nodiscard]] math::Double2 OutwardNormal(
    const world::TileEdge edge) noexcept
{
    switch (edge)
    {
    case world::TileEdge::North:
        return {0.0, -1.0};
    case world::TileEdge::East:
        return {1.0, 0.0};
    case world::TileEdge::South:
        return {0.0, 1.0};
    case world::TileEdge::West:
        return {-1.0, 0.0};
    }

    return {};
}

[[nodiscard]] math::Double2 PositiveTangent(
    const world::TileEdge edge) noexcept
{
    switch (edge)
    {
    case world::TileEdge::North:
    case world::TileEdge::South:
        return {1.0, 0.0};
    case world::TileEdge::East:
    case world::TileEdge::West:
        return {0.0, 1.0};
    }

    return {};
}

[[nodiscard]] std::pair<i32, i32> CornerOffset(
    const SurfaceBoundaryCorner corner) noexcept
{
    switch (corner)
    {
    case SurfaceBoundaryCorner::NorthWest:
        return {-1, -1};
    case SurfaceBoundaryCorner::NorthEast:
        return {1, -1};
    case SurfaceBoundaryCorner::SouthEast:
        return {1, 1};
    case SurfaceBoundaryCorner::SouthWest:
        return {-1, 1};
    }

    return {};
}

[[nodiscard]] math::Double2 CornerUv(
    const world::CubeBounds& bounds,
    const SurfaceBoundaryCorner corner) noexcept
{
    switch (corner)
    {
    case SurfaceBoundaryCorner::NorthWest:
        return bounds.minimumUv;
    case SurfaceBoundaryCorner::NorthEast:
        return {
            bounds.maximumUv.x,
            bounds.minimumUv.y};
    case SurfaceBoundaryCorner::SouthEast:
        return bounds.maximumUv;
    case SurfaceBoundaryCorner::SouthWest:
        return {
            bounds.minimumUv.x,
            bounds.maximumUv.y};
    }

    return bounds.minimumUv;
}

[[nodiscard]] SurfaceBoundaryCorner ClosestCorner(
    const world::PlanetTileId& tile,
    const math::Double3& direction) noexcept
{
    const world::CubeBounds bounds =
        world::TileBounds(tile);

    f64 bestDot = -2.0;
    SurfaceBoundaryCorner best =
        SurfaceBoundaryCorner::NorthWest;

    for (u8 raw = 0U; raw < 4U; ++raw)
    {
        const auto corner =
            static_cast<SurfaceBoundaryCorner>(raw);

        const math::Double3 candidate =
            world::CubeToUnitDirection({
                .face = tile.face,
                .uv = CornerUv(bounds, corner)
            });

        const f64 dot =
            math::Dot(candidate, direction);

        if (dot > bestDot)
        {
            bestDot = dot;
            best = corner;
        }
    }

    return best;
}

[[nodiscard]] world::TileEdge PrimaryCornerEdge(
    const SurfaceBoundaryCorner corner) noexcept
{
    switch (corner)
    {
    case SurfaceBoundaryCorner::NorthWest:
    case SurfaceBoundaryCorner::NorthEast:
        return world::TileEdge::North;
    case SurfaceBoundaryCorner::SouthEast:
    case SurfaceBoundaryCorner::SouthWest:
        return world::TileEdge::South;
    }

    return world::TileEdge::North;
}

[[nodiscard]] std::optional<world::TileEdgeNeighborMapping>
MappingFromTileToTarget(
    const world::PlanetTileId& tile,
    const world::PlanetTileId& target) noexcept
{
    for (u8 raw = 0U; raw < 4U; ++raw)
    {
        const auto edge =
            static_cast<world::TileEdge>(raw);

        const auto mapping =
            world::NeighborAcrossTileEdge(
                tile,
                edge);

        if (mapping.tile == target)
        {
            return mapping;
        }
    }

    return std::nullopt;
}

[[nodiscard]] math::Double2 TransformCornerVector(
    const world::PlanetTileId& source,
    const world::PlanetTileId& target,
    const SurfaceBoundaryCorner corner,
    const math::Double2 vector) noexcept
{
    const world::TileEdge firstEdge =
        PrimaryCornerEdge(corner);

    const auto first =
        world::NeighborAcrossTileEdge(
            source,
            firstEdge);

    math::Double2 transformed =
        TransformBoundaryVectorAcrossEdge(
            firstEdge,
            first,
            vector);

    if (first.tile == target)
    {
        return transformed;
    }

    const auto second =
        MappingFromTileToTarget(
            first.tile,
            target);

    if (!second)
    {
        return {};
    }

    world::TileEdge secondSourceEdge =
        world::TileEdge::North;

    for (u8 raw = 0U; raw < 4U; ++raw)
    {
        const auto candidate =
            static_cast<world::TileEdge>(raw);

        const auto mapping =
            world::NeighborAcrossTileEdge(
                first.tile,
                candidate);

        if (mapping.tile == target)
        {
            secondSourceEdge = candidate;
            break;
        }
    }

    return TransformBoundaryVectorAcrossEdge(
        secondSourceEdge,
        *second,
        transformed);
}

void TransformSedimentPacketAcrossEdge(
    terrain_erosion::SedimentTransportPacket& packet,
    const world::TileEdge sourceEdge,
    const world::TileEdgeNeighborMapping& mapping) noexcept
{
    const auto transform =
        [&](terrain_erosion::SedimentTransportVector& vector)
        {
            const math::Double2 remapped =
                TransformBoundaryVectorAcrossEdge(
                    sourceEdge,
                    mapping,
                    {
                        vector.eastKg,
                        -vector.northKg
                    });

            vector.eastKg =
                remapped.x;
            vector.northKg =
                -remapped.y;
        };

    transform(packet.waterborneTransport);
    transform(packet.airborneTransport);
    transform(packet.surfaceMobileTransport);
}

void TransformSedimentPacketAtCorner(
    terrain_erosion::SedimentTransportPacket& packet,
    const world::PlanetTileId& source,
    const world::PlanetTileId& target,
    const SurfaceBoundaryCorner corner) noexcept
{
    const auto transform =
        [&](terrain_erosion::SedimentTransportVector& vector)
        {
            const math::Double2 remapped =
                TransformCornerVector(
                    source,
                    target,
                    corner,
                    {
                        vector.eastKg,
                        -vector.northKg
                    });

            vector.eastKg =
                remapped.x;
            vector.northKg =
                -remapped.y;
        };

    transform(packet.waterborneTransport);
    transform(packet.airborneTransport);
    transform(packet.surfaceMobileTransport);
}

[[nodiscard]] terrain_hydrology::DrainageBoundaryCell
DrainageBoundaryFromCell(
    const terrain_hydrology::DrainageCell& cell) noexcept
{
    return {
        .surfaceHeightMeters = cell.surfaceHeightMeters,
        .conditionedHeightMeters =
            cell.drainageElevationMeters,
        .authoredDrainage = cell.authoredDrainage,
        .drainageAreaSquareMeters =
            cell.drainageAreaSquareMeters,
        .dischargeCubicMetersPerSecond =
            cell.dischargeCubicMetersPerSecond,
        .flowDx = cell.flow.dx,
        .flowDy = cell.flow.dy
    };
}

[[nodiscard]] terrain_water::CoastalBoundaryCell
CoastalBoundaryFromCell(
    const terrain_water::CoastalCellState& cell) noexcept
{
    return {
        .mode = terrain_water::CoastalBoundaryMode::Neighbor,
        .bedElevationMeters =
            static_cast<f32>(cell.bedElevationMeters),
        .waterSurfaceElevationMeters =
            static_cast<f32>(
                cell.waterSurfaceElevationMeters),
        .velocityEastMetersPerSecond =
            static_cast<f32>(
                cell.velocityMetersPerSecond.x),
        .velocitySouthMetersPerSecond =
            static_cast<f32>(
                cell.velocityMetersPerSecond.y)
    };
}

void TransformGhostVectorsAcrossEdge(
    SurfaceGhostCell& cell,
    const world::TileEdge sourceEdge,
    const world::TileEdgeNeighborMapping& mapping) noexcept
{
    if (cell.drainage)
    {
        const math::Double2 transformed =
            TransformBoundaryVectorAcrossEdge(
                sourceEdge,
                mapping,
                {
                    static_cast<f64>(
                        cell.drainage->flowDx),
                    static_cast<f64>(
                        cell.drainage->flowDy)
                });

        cell.drainage->flowDx =
            static_cast<i8>(
                std::clamp(
                    static_cast<i32>(
                        std::lround(transformed.x)),
                    -1,
                    1));

        cell.drainage->flowDy =
            static_cast<i8>(
                std::clamp(
                    static_cast<i32>(
                        std::lround(transformed.y)),
                    -1,
                    1));
    }

    if (cell.water)
    {
        const math::Double2 transformed =
            TransformBoundaryVectorAcrossEdge(
                sourceEdge,
                mapping,
                {
                    cell.water->
                        velocityEastMetersPerSecond,
                    cell.water->
                        velocitySouthMetersPerSecond
                });

        cell.water->velocityEastMetersPerSecond =
            static_cast<f32>(transformed.x);
        cell.water->velocitySouthMetersPerSecond =
            static_cast<f32>(transformed.y);
        cell.water->mode =
            terrain_water::CoastalBoundaryMode::Neighbor;
    }
}

void TransformGhostVectorsAtCorner(
    SurfaceGhostCell& cell,
    const world::PlanetTileId& source,
    const world::PlanetTileId& target,
    const SurfaceBoundaryCorner corner) noexcept
{
    if (cell.drainage)
    {
        const math::Double2 transformed =
            TransformCornerVector(
                source,
                target,
                corner,
                {
                    static_cast<f64>(
                        cell.drainage->flowDx),
                    static_cast<f64>(
                        cell.drainage->flowDy)
                });

        cell.drainage->flowDx =
            static_cast<i8>(
                std::clamp(
                    static_cast<i32>(
                        std::lround(transformed.x)),
                    -1,
                    1));

        cell.drainage->flowDy =
            static_cast<i8>(
                std::clamp(
                    static_cast<i32>(
                        std::lround(transformed.y)),
                    -1,
                    1));
    }

    if (cell.water)
    {
        const math::Double2 transformed =
            TransformCornerVector(
                source,
                target,
                corner,
                {
                    cell.water->
                        velocityEastMetersPerSecond,
                    cell.water->
                        velocitySouthMetersPerSecond
                });

        cell.water->velocityEastMetersPerSecond =
            static_cast<f32>(transformed.x);
        cell.water->velocitySouthMetersPerSecond =
            static_cast<f32>(transformed.y);
        cell.water->mode =
            terrain_water::CoastalBoundaryMode::Neighbor;
    }
}

[[nodiscard]] std::size_t CornerIndex(
    const SurfaceBoundaryCorner corner) noexcept
{
    return static_cast<std::size_t>(corner);
}

[[nodiscard]] bool EdgeTransferLess(
    const SurfaceBoundaryEdgeTransfer& a,
    const SurfaceBoundaryEdgeTransfer& b) noexcept
{
    if (AddressLess(a.target, b.target))
    {
        return true;
    }
    if (AddressLess(b.target, a.target))
    {
        return false;
    }

    if (a.targetEdge != b.targetEdge)
    {
        return
            static_cast<u8>(a.targetEdge) <
            static_cast<u8>(b.targetEdge);
    }

    return AddressLess(a.source, b.source);
}

[[nodiscard]] bool GhostEdgeTransferLess(
    const SurfaceGhostEdgeTransfer& a,
    const SurfaceGhostEdgeTransfer& b) noexcept
{
    if (AddressLess(a.target, b.target))
    {
        return true;
    }
    if (AddressLess(b.target, a.target))
    {
        return false;
    }

    if (a.targetEdge != b.targetEdge)
    {
        return
            static_cast<u8>(a.targetEdge) <
            static_cast<u8>(b.targetEdge);
    }

    return AddressLess(a.source, b.source);
}

[[nodiscard]] bool CornerTransferLess(
    const SurfaceBoundaryCornerTransfer& a,
    const SurfaceBoundaryCornerTransfer& b) noexcept
{
    if (AddressLess(a.target, b.target))
    {
        return true;
    }
    if (AddressLess(b.target, a.target))
    {
        return false;
    }

    if (a.targetCorner != b.targetCorner)
    {
        return
            static_cast<u8>(a.targetCorner) <
            static_cast<u8>(b.targetCorner);
    }

    return AddressLess(a.source, b.source);
}

[[nodiscard]] bool GhostCornerTransferLess(
    const SurfaceGhostCornerTransfer& a,
    const SurfaceGhostCornerTransfer& b) noexcept
{
    if (AddressLess(a.target, b.target))
    {
        return true;
    }
    if (AddressLess(b.target, a.target))
    {
        return false;
    }

    if (a.targetCorner != b.targetCorner)
    {
        return
            static_cast<u8>(a.targetCorner) <
            static_cast<u8>(b.targetCorner);
    }

    return AddressLess(a.source, b.source);
}

[[nodiscard]] u64 CombinedRevision(
    const u64 current,
    const terrain::PhysicalTerrainPageAddress& source,
    const u64 revision) noexcept
{
    u64 value = current;
    value = terrain::StableCombine64(
        value,
        source.planet.high);
    value = terrain::StableCombine64(
        value,
        source.planet.low);
    value = terrain::StableCombine64(
        value,
        static_cast<u64>(source.tile.face));
    value = terrain::StableCombine64(
        value,
        source.tile.level);
    value = terrain::StableCombine64(
        value,
        source.tile.x);
    value = terrain::StableCombine64(
        value,
        source.tile.y);
    value = terrain::StableCombine64(
        value,
        revision);
    return value;
}

template <typename Transfer>
[[nodiscard]] std::vector<const Transfer*> MatchingEdges(
    const terrain::PhysicalTerrainPageAddress& receiver,
    const std::vector<Transfer>& transfers)
{
    std::vector<const Transfer*> result;
    for (const auto& transfer : transfers)
    {
        if (SameAddress(
                transfer.target,
                receiver))
        {
            result.push_back(&transfer);
        }
    }
    return result;
}

template <typename Transfer>
[[nodiscard]] std::vector<const Transfer*> MatchingCorners(
    const terrain::PhysicalTerrainPageAddress& receiver,
    const std::vector<Transfer>& transfers)
{
    std::vector<const Transfer*> result;
    for (const auto& transfer : transfers)
    {
        if (SameAddress(
                transfer.target,
                receiver))
        {
            result.push_back(&transfer);
        }
    }
    return result;
}
} // namespace

bool WaterBoundaryFluxPacket::IsValid() const noexcept
{
    return
        Finite(volumeCubicMeters) &&
        volumeCubicMeters >= 0.0 &&
        Finite(velocityMoment.x) &&
        Finite(velocityMoment.y);
}

WaterBoundaryFluxPacket&
WaterBoundaryFluxPacket::operator+=(
    const WaterBoundaryFluxPacket& other) noexcept
{
    volumeCubicMeters +=
        other.volumeCubicMeters;
    velocityMoment =
        velocityMoment +
        other.velocityMoment;
    return *this;
}

WaterBoundaryFluxPacket operator+(
    WaterBoundaryFluxPacket left,
    const WaterBoundaryFluxPacket& right) noexcept
{
    left += right;
    return left;
}

bool SurfaceBoundaryFlux::IsComplete() const noexcept
{
    if (resolution == 0U ||
        north.size() != resolution ||
        east.size() != resolution ||
        south.size() != resolution ||
        west.size() != resolution ||
        !sediment.IsComplete(resolution))
    {
        return false;
    }

    const auto waterValid =
        [](const std::vector<WaterBoundaryFluxPacket>& edge)
        {
            return std::all_of(
                edge.begin(),
                edge.end(),
                [](const WaterBoundaryFluxPacket& packet)
                {
                    return packet.IsValid();
                });
        };

    if (!waterValid(north) ||
        !waterValid(east) ||
        !waterValid(south) ||
        !waterValid(west))
    {
        return false;
    }

    for (const auto& corner : corners)
    {
        if (!corner.IsValid())
        {
            return false;
        }
    }

    return sediment.Total().IsValid();
}

f64 SurfaceBoundaryFlux::TotalWaterVolumeCubicMeters() const noexcept
{
    f64 total = 0.0;

    const auto add =
        [&total](const std::vector<WaterBoundaryFluxPacket>& edge)
        {
            for (const auto& packet : edge)
            {
                total += packet.volumeCubicMeters;
            }
        };

    add(north);
    add(east);
    add(south);
    add(west);

    for (const auto& corner : corners)
    {
        total += corner.volumeCubicMeters;
    }

    return total;
}

terrain_erosion::SedimentMass
SurfaceBoundaryFlux::TotalSediment() const noexcept
{
    return sediment.Total();
}

SurfaceBoundaryFlux MakeSurfaceBoundaryFlux(
    const u32 resolution,
    const u64 revision,
    terrain_erosion::SedimentBoundaryFlux sediment)
{
    if (resolution == 0U)
    {
        throw std::invalid_argument(
            "M25 surface boundary flux resolution must be nonzero.");
    }

    if (sediment.north.empty() &&
        sediment.east.empty() &&
        sediment.south.empty() &&
        sediment.west.empty())
    {
        sediment.north.resize(resolution);
        sediment.east.resize(resolution);
        sediment.south.resize(resolution);
        sediment.west.resize(resolution);
    }

    if (!sediment.IsComplete(resolution))
    {
        throw std::invalid_argument(
            "M25 sediment boundary flux is incomplete.");
    }

    sediment.revision = revision;

    SurfaceBoundaryFlux result{
        .resolution = resolution,
        .revision = revision,
        .north =
            std::vector<WaterBoundaryFluxPacket>(resolution),
        .east =
            std::vector<WaterBoundaryFluxPacket>(resolution),
        .south =
            std::vector<WaterBoundaryFluxPacket>(resolution),
        .west =
            std::vector<WaterBoundaryFluxPacket>(resolution),
        .sediment = std::move(sediment)
    };

    return result;
}

bool PhysicalPageBoundaryFlux::IsValid() const noexcept
{
    return
        address.planet.IsValid() &&
        outgoing.IsComplete();
}

f64 SurfaceBoundaryTransferBatch::TotalWaterVolumeCubicMeters() const noexcept
{
    f64 total = 0.0;

    for (const auto& transfer : edges)
    {
        for (const auto& packet : transfer.water)
        {
            total += packet.volumeCubicMeters;
        }
    }

    for (const auto& transfer : corners)
    {
        total += transfer.water.volumeCubicMeters;
    }

    return total;
}

terrain_erosion::SedimentMass
SurfaceBoundaryTransferBatch::TotalSediment() const noexcept
{
    terrain_erosion::SedimentMass total{};

    for (const auto& transfer : edges)
    {
        for (const auto& packet : transfer.sediment)
        {
            total += packet.Total();
        }
    }

    for (const auto& transfer : corners)
    {
        total += transfer.sediment.Total();
    }

    return total;
}

math::Double2 TransformBoundaryVectorAcrossEdge(
    const world::TileEdge sourceEdge,
    const world::TileEdgeNeighborMapping& mapping,
    const math::Double2 sourceVector) noexcept
{
    const math::Double2 sourceNormal =
        OutwardNormal(sourceEdge);
    const math::Double2 sourceTangent =
        PositiveTangent(sourceEdge);

    const f64 normal =
        sourceVector.x * sourceNormal.x +
        sourceVector.y * sourceNormal.y;

    f64 tangent =
        sourceVector.x * sourceTangent.x +
        sourceVector.y * sourceTangent.y;

    if (mapping.reverseSamples)
    {
        tangent = -tangent;
    }

    const math::Double2 targetInward =
        OutwardNormal(mapping.edge) * -1.0;

    const math::Double2 targetTangent =
        PositiveTangent(mapping.edge);

    return
        targetInward * normal +
        targetTangent * tangent;
}

SurfaceBoundaryTransferBatch
BuildDeterministicBoundaryTransfers(
    const std::span<const PhysicalPageBoundaryFlux> pages)
{
    SurfaceBoundaryTransferBatch result;

    std::vector<const PhysicalPageBoundaryFlux*> ordered;
    ordered.reserve(pages.size());

    for (const auto& page : pages)
    {
        if (!page.IsValid())
        {
            throw std::invalid_argument(
                "M25 boundary page flux is invalid.");
        }

        ordered.push_back(&page);
    }

    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const auto* a, const auto* b)
        {
            return AddressLess(
                a->address,
                b->address);
        });

    for (std::size_t index = 1U;
         index < ordered.size();
         ++index)
    {
        if (SameAddress(
                ordered[index - 1U]->address,
                ordered[index]->address))
        {
            throw std::invalid_argument(
                "M25 received duplicate physical page boundary flux.");
        }
    }

    result.edges.reserve(ordered.size() * 4U);
    result.corners.reserve(ordered.size() * 4U);

    for (const auto* page : ordered)
    {
        for (u8 raw = 0U; raw < 4U; ++raw)
        {
            const auto edge =
                static_cast<world::TileEdge>(raw);

            const auto mapping =
                world::NeighborAcrossTileEdge(
                    page->address.tile,
                    edge);

            SurfaceBoundaryEdgeTransfer transfer{
                .source = page->address,
                .target = {
                    .planet = page->address.planet,
                    .tile = mapping.tile
                },
                .sourceEdge = edge,
                .targetEdge = mapping.edge,
                .reversed = mapping.reverseSamples,
                .revision = page->outgoing.revision,
                .water = std::vector<WaterBoundaryFluxPacket>(
                    page->outgoing.resolution),
                .sediment =
                    std::vector<
                        terrain_erosion::SedimentTransportPacket>(
                            page->outgoing.resolution)
            };

            const auto& waterSource =
                WaterEdge(
                    page->outgoing,
                    edge);

            const auto& sedimentSource =
                SedimentEdge(
                    page->outgoing.sediment,
                    edge);

            for (u32 sourceIndex = 0U;
                 sourceIndex < page->outgoing.resolution;
                 ++sourceIndex)
            {
                const u32 targetIndex =
                    world::RemapTileEdgeSampleIndex(
                        mapping,
                        sourceIndex,
                        page->outgoing.resolution);

                WaterBoundaryFluxPacket water =
                    waterSource[sourceIndex];

                water.velocityMoment =
                    TransformBoundaryVectorAcrossEdge(
                        edge,
                        mapping,
                        water.velocityMoment);

                transfer.water[targetIndex] =
                    water;

                auto sedimentPacket =
                    sedimentSource[sourceIndex];

                TransformSedimentPacketAcrossEdge(
                    sedimentPacket,
                    edge,
                    mapping);

                transfer.sediment[targetIndex] =
                    sedimentPacket;
            }

            result.edges.push_back(
                std::move(transfer));
        }

        const world::CubeBounds sourceBounds =
            world::TileBounds(
                page->address.tile);

        for (u8 raw = 0U; raw < 4U; ++raw)
        {
            const auto corner =
                static_cast<SurfaceBoundaryCorner>(raw);

            const auto [dx, dy] =
                CornerOffset(corner);

            const world::PlanetTileId targetTile =
                world::OffsetTile(
                    page->address.tile,
                    dx,
                    dy);

            const math::Double3 cornerDirection =
                world::CubeToUnitDirection({
                    .face = page->address.tile.face,
                    .uv = CornerUv(
                        sourceBounds,
                        corner)
                });

            const SurfaceBoundaryCorner targetCorner =
                ClosestCorner(
                    targetTile,
                    cornerDirection);

            const std::size_t sourceCornerIndex =
                CornerIndex(corner);

            WaterBoundaryFluxPacket water =
                page->outgoing.
                    corners[sourceCornerIndex];

            water.velocityMoment =
                TransformCornerVector(
                    page->address.tile,
                    targetTile,
                    corner,
                    water.velocityMoment);

            auto sedimentPacket =
                page->outgoing.sediment.
                    corners[sourceCornerIndex];

            TransformSedimentPacketAtCorner(
                sedimentPacket,
                page->address.tile,
                targetTile,
                corner);

            result.corners.push_back({
                .source = page->address,
                .target = {
                    .planet = page->address.planet,
                    .tile = targetTile
                },
                .sourceCorner = corner,
                .targetCorner = targetCorner,
                .revision = page->outgoing.revision,
                .water = water,
                .sediment =
                    sedimentPacket
            });
        }
    }

    std::sort(
        result.edges.begin(),
        result.edges.end(),
        EdgeTransferLess);

    std::sort(
        result.corners.begin(),
        result.corners.end(),
        CornerTransferLess);

    return result;
}

void ApplySedimentBoundaryTransfers(
    const terrain::PhysicalTerrainPageAddress& receiver,
    const SurfaceBoundaryTransferBatch& transfers,
    terrain_erosion::SedimentExchangePage& sediment)
{
    terrain_erosion::SedimentBoundaryFlux incoming{
        .north =
            std::vector<
                terrain_erosion::SedimentTransportPacket>(
                    sediment.Resolution()),
        .east =
            std::vector<
                terrain_erosion::SedimentTransportPacket>(
                    sediment.Resolution()),
        .south =
            std::vector<
                terrain_erosion::SedimentTransportPacket>(
                    sediment.Resolution()),
        .west =
            std::vector<
                terrain_erosion::SedimentTransportPacket>(
                    sediment.Resolution())
    };

    u64 revision =
        0x4D3235424F554E44ULL; // "M25BOUND"

    for (const auto* transfer :
         MatchingEdges(receiver, transfers.edges))
    {
        if (transfer->sediment.size() !=
            sediment.Resolution())
        {
            throw std::invalid_argument(
                "M25 sediment edge resolution mismatch.");
        }

        auto& target =
            SedimentEdge(
                incoming,
                transfer->targetEdge);

        for (u32 index = 0U;
             index < sediment.Resolution();
             ++index)
        {
            target[index].waterborne +=
                transfer->sediment[index].waterborne;
            target[index].airborne +=
                transfer->sediment[index].airborne;
            target[index].surfaceMobile +=
                transfer->sediment[index].surfaceMobile;

            target[index].waterborneTransport +=
                transfer->sediment[index].
                    waterborneTransport;

            target[index].airborneTransport +=
                transfer->sediment[index].
                    airborneTransport;

            target[index].surfaceMobileTransport +=
                transfer->sediment[index].
                    surfaceMobileTransport;
        }

        revision =
            CombinedRevision(
                revision,
                transfer->source,
                transfer->revision);
    }

    for (const auto* transfer :
         MatchingCorners(receiver, transfers.corners))
    {
        auto& target =
            incoming.corners[
                CornerIndex(
                    transfer->targetCorner)];

        target.waterborne +=
            transfer->sediment.waterborne;
        target.airborne +=
            transfer->sediment.airborne;
        target.surfaceMobile +=
            transfer->sediment.surfaceMobile;

        target.waterborneTransport +=
            transfer->sediment.
                waterborneTransport;

        target.airborneTransport +=
            transfer->sediment.
                airborneTransport;

        target.surfaceMobileTransport +=
            transfer->sediment.
                surfaceMobileTransport;

        revision =
            CombinedRevision(
                revision,
                transfer->source,
                transfer->revision);
    }

    incoming.revision = revision;
    sediment.ImportBoundaryFlux(incoming);
}

WaterBoundaryFluxPacket
IncomingWaterBoundaryFlux(
    const terrain::PhysicalTerrainPageAddress& receiver,
    const SurfaceBoundaryTransferBatch& transfers) noexcept
{
    WaterBoundaryFluxPacket result{};

    for (const auto& transfer : transfers.edges)
    {
        if (!SameAddress(
                transfer.target,
                receiver))
        {
            continue;
        }

        for (const auto& packet : transfer.water)
        {
            result += packet;
        }
    }

    for (const auto& transfer : transfers.corners)
    {
        if (SameAddress(
                transfer.target,
                receiver))
        {
            result += transfer.water;
        }
    }

    return result;
}

bool SurfaceGhostCell::IsValid() const noexcept
{
    if (!material.IsValid())
    {
        return false;
    }

    if (drainage &&
        !drainage->IsValid())
    {
        return false;
    }

    if (water &&
        !water->IsValid())
    {
        return false;
    }

    return true;
}

bool SurfaceBoundarySnapshot::IsComplete() const noexcept
{
    if (!address.planet.IsValid() ||
        resolution == 0U ||
        !Finite(spacingMeters) ||
        spacingMeters <= 0.0 ||
        north.size() != resolution ||
        east.size() != resolution ||
        south.size() != resolution ||
        west.size() != resolution)
    {
        return false;
    }

    const auto validEdge =
        [](const std::vector<SurfaceGhostCell>& edge)
        {
            return std::all_of(
                edge.begin(),
                edge.end(),
                [](const SurfaceGhostCell& cell)
                {
                    return cell.IsValid();
                });
        };

    if (!validEdge(north) ||
        !validEdge(east) ||
        !validEdge(south) ||
        !validEdge(west))
    {
        return false;
    }

    return std::all_of(
        corners.begin(),
        corners.end(),
        [](const SurfaceGhostCell& cell)
        {
            return cell.IsValid();
        });
}

SurfaceBoundarySnapshot BuildSurfaceBoundarySnapshot(
    const terrain::PhysicalTerrainPageAddress& address,
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_hydrology::DrainagePage* drainage,
    const terrain_water::CoastalWaterPage* water,
    const u64 revision)
{
    if (!address.planet.IsValid())
    {
        throw std::invalid_argument(
            "M25 surface boundary snapshot address is invalid.");
    }

    const u32 resolution =
        material.Resolution();

    if (drainage != nullptr)
    {
        if (drainage->Resolution() != resolution ||
            drainage->SourcePage().address != address)
        {
            throw std::invalid_argument(
                "M25 drainage snapshot does not match material page.");
        }
    }

    if (water != nullptr &&
        (water->resolution != resolution ||
         std::abs(
             water->spacingMeters -
             material.SpacingMeters()) > 1.0e-9))
    {
        throw std::invalid_argument(
            "M25 coastal snapshot does not match material page.");
    }

    SurfaceBoundarySnapshot result{
        .address = address,
        .resolution = resolution,
        .spacingMeters = material.SpacingMeters(),
        .revision = revision,
        .north = std::vector<SurfaceGhostCell>(resolution),
        .east = std::vector<SurfaceGhostCell>(resolution),
        .south = std::vector<SurfaceGhostCell>(resolution),
        .west = std::vector<SurfaceGhostCell>(resolution)
    };

    const auto makeCell =
        [&](const u32 x, const u32 y)
        {
            SurfaceGhostCell cell{
                .material = material.At(x, y)
            };

            if (drainage != nullptr)
            {
                cell.drainage =
                    DrainageBoundaryFromCell(
                        drainage->At(x, y));
            }

            if (water != nullptr)
            {
                cell.water =
                    CoastalBoundaryFromCell(
                        water->At(x, y));
            }

            return cell;
        };

    for (u32 index = 0U;
         index < resolution;
         ++index)
    {
        result.north[index] =
            makeCell(index, 0U);
        result.east[index] =
            makeCell(
                resolution - 1U,
                index);
        result.south[index] =
            makeCell(
                index,
                resolution - 1U);
        result.west[index] =
            makeCell(0U, index);
    }

    result.corners[0] =
        makeCell(0U, 0U);
    result.corners[1] =
        makeCell(
            resolution - 1U,
            0U);
    result.corners[2] =
        makeCell(
            resolution - 1U,
            resolution - 1U);
    result.corners[3] =
        makeCell(
            0U,
            resolution - 1U);

    return result;
}

SurfaceGhostTransferBatch
BuildDeterministicGhostTransfers(
    const std::span<const SurfaceBoundarySnapshot> pages)
{
    SurfaceGhostTransferBatch result;

    std::vector<const SurfaceBoundarySnapshot*> ordered;
    ordered.reserve(pages.size());

    for (const auto& page : pages)
    {
        if (!page.IsComplete())
        {
            throw std::invalid_argument(
                "M25 physical boundary snapshot is incomplete.");
        }

        ordered.push_back(&page);
    }

    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const auto* a, const auto* b)
        {
            return AddressLess(
                a->address,
                b->address);
        });

    for (std::size_t index = 1U;
         index < ordered.size();
         ++index)
    {
        if (SameAddress(
                ordered[index - 1U]->address,
                ordered[index]->address))
        {
            throw std::invalid_argument(
                "M25 received duplicate physical boundary snapshots.");
        }
    }

    result.edges.reserve(ordered.size() * 4U);
    result.corners.reserve(ordered.size() * 4U);

    for (const auto* page : ordered)
    {
        for (u8 raw = 0U; raw < 4U; ++raw)
        {
            const auto edge =
                static_cast<world::TileEdge>(raw);

            const auto mapping =
                world::NeighborAcrossTileEdge(
                    page->address.tile,
                    edge);

            SurfaceGhostEdgeTransfer transfer{
                .source = page->address,
                .target = {
                    .planet = page->address.planet,
                    .tile = mapping.tile
                },
                .sourceEdge = edge,
                .targetEdge = mapping.edge,
                .reversed = mapping.reverseSamples,
                .revision = page->revision,
                .cells =
                    std::vector<SurfaceGhostCell>(
                        page->resolution)
            };

            const auto& source =
                GhostEdge(*page, edge);

            for (u32 sourceIndex = 0U;
                 sourceIndex < page->resolution;
                 ++sourceIndex)
            {
                const u32 targetIndex =
                    world::RemapTileEdgeSampleIndex(
                        mapping,
                        sourceIndex,
                        page->resolution);

                SurfaceGhostCell cell =
                    source[sourceIndex];

                TransformGhostVectorsAcrossEdge(
                    cell,
                    edge,
                    mapping);

                transfer.cells[targetIndex] =
                    std::move(cell);
            }

            result.edges.push_back(
                std::move(transfer));
        }

        const world::CubeBounds sourceBounds =
            world::TileBounds(
                page->address.tile);

        for (u8 raw = 0U; raw < 4U; ++raw)
        {
            const auto corner =
                static_cast<SurfaceBoundaryCorner>(raw);

            const auto [dx, dy] =
                CornerOffset(corner);

            const world::PlanetTileId targetTile =
                world::OffsetTile(
                    page->address.tile,
                    dx,
                    dy);

            const math::Double3 direction =
                world::CubeToUnitDirection({
                    .face = page->address.tile.face,
                    .uv = CornerUv(
                        sourceBounds,
                        corner)
                });

            const auto targetCorner =
                ClosestCorner(
                    targetTile,
                    direction);

            SurfaceGhostCell cell =
                page->corners[
                    CornerIndex(corner)];

            TransformGhostVectorsAtCorner(
                cell,
                page->address.tile,
                targetTile,
                corner);

            result.corners.push_back({
                .source = page->address,
                .target = {
                    .planet = page->address.planet,
                    .tile = targetTile
                },
                .sourceCorner = corner,
                .targetCorner = targetCorner,
                .revision = page->revision,
                .cell = std::move(cell)
            });
        }
    }

    std::sort(
        result.edges.begin(),
        result.edges.end(),
        GhostEdgeTransferLess);

    std::sort(
        result.corners.begin(),
        result.corners.end(),
        GhostCornerTransferLess);

    return result;
}

terrain_hydrology::DrainagePageHalo
BuildDrainageHaloFromGhostTransfers(
    const terrain::PhysicalTerrainPageAddress& receiver,
    const u32 resolution,
    const SurfaceGhostTransferBatch& transfers)
{
    if (resolution == 0U)
    {
        throw std::invalid_argument(
            "M25 drainage halo resolution must be nonzero.");
    }

    terrain_hydrology::DrainagePageHalo halo{
        .north =
            std::vector<
                terrain_hydrology::DrainageBoundaryCell>(
                    resolution),
        .east =
            std::vector<
                terrain_hydrology::DrainageBoundaryCell>(
                    resolution),
        .south =
            std::vector<
                terrain_hydrology::DrainageBoundaryCell>(
                    resolution),
        .west =
            std::vector<
                terrain_hydrology::DrainageBoundaryCell>(
                    resolution)
    };

    std::array<bool, 4> haveEdge{};
    std::array<bool, 4> haveCorner{};

    u64 revision =
        0x4D3235445241494EULL; // "M25DRAIN"

    for (const auto* transfer :
         MatchingEdges(receiver, transfers.edges))
    {
        const std::size_t edgeIndex =
            static_cast<std::size_t>(
                transfer->targetEdge);

        if (edgeIndex >= haveEdge.size() ||
            haveEdge[edgeIndex] ||
            transfer->cells.size() != resolution)
        {
            throw std::invalid_argument(
                "M25 drainage halo has duplicate/mismatched edge state.");
        }

        std::vector<
            terrain_hydrology::DrainageBoundaryCell>* target =
            nullptr;

        switch (transfer->targetEdge)
        {
        case world::TileEdge::North:
            target = &halo.north;
            break;
        case world::TileEdge::East:
            target = &halo.east;
            break;
        case world::TileEdge::South:
            target = &halo.south;
            break;
        case world::TileEdge::West:
            target = &halo.west;
            break;
        }

        for (u32 index = 0U;
             index < resolution;
             ++index)
        {
            if (!transfer->cells[index].drainage)
            {
                throw std::invalid_argument(
                    "M25 drainage halo source lacks M09 state.");
            }

            (*target)[index] =
                *transfer->cells[index].drainage;
        }

        haveEdge[edgeIndex] = true;

        revision =
            CombinedRevision(
                revision,
                transfer->source,
                transfer->revision);
    }

    for (const auto* transfer :
         MatchingCorners(receiver, transfers.corners))
    {
        const std::size_t cornerIndex =
            CornerIndex(
                transfer->targetCorner);

        if (cornerIndex >= haveCorner.size() ||
            haveCorner[cornerIndex] ||
            !transfer->cell.drainage)
        {
            throw std::invalid_argument(
                "M25 drainage halo has duplicate/missing corner state.");
        }

        halo.corners[cornerIndex] =
            *transfer->cell.drainage;

        haveCorner[cornerIndex] = true;

        revision =
            CombinedRevision(
                revision,
                transfer->source,
                transfer->revision);
    }

    if (!std::all_of(
            haveEdge.begin(),
            haveEdge.end(),
            [](const bool value)
            {
                return value;
            }) ||
        !std::all_of(
            haveCorner.begin(),
            haveCorner.end(),
            [](const bool value)
            {
                return value;
            }))
    {
        throw std::invalid_argument(
            "M25 drainage halo requires all physical neighbors.");
    }

    halo.revision = revision;
    return halo;
}

terrain_water::CoastalBoundaryState
BuildCoastalBoundaryFromGhostTransfers(
    const terrain::PhysicalTerrainPageAddress& receiver,
    const u32 resolution,
    const SurfaceGhostTransferBatch& transfers)
{
    if (resolution == 0U)
    {
        throw std::invalid_argument(
            "M25 coastal boundary resolution must be nonzero.");
    }

    terrain_water::CoastalBoundaryState boundary{
        .north =
            std::vector<
                terrain_water::CoastalBoundaryCell>(
                    resolution),
        .east =
            std::vector<
                terrain_water::CoastalBoundaryCell>(
                    resolution),
        .south =
            std::vector<
                terrain_water::CoastalBoundaryCell>(
                    resolution),
        .west =
            std::vector<
                terrain_water::CoastalBoundaryCell>(
                    resolution)
    };

    std::array<bool, 4> haveEdge{};

    u64 revision =
        0x4D32355741544552ULL; // "M25WATER"

    for (const auto* transfer :
         MatchingEdges(receiver, transfers.edges))
    {
        const std::size_t edgeIndex =
            static_cast<std::size_t>(
                transfer->targetEdge);

        if (edgeIndex >= haveEdge.size() ||
            haveEdge[edgeIndex] ||
            transfer->cells.size() != resolution)
        {
            throw std::invalid_argument(
                "M25 coastal boundary has duplicate/mismatched edge state.");
        }

        std::vector<
            terrain_water::CoastalBoundaryCell>* target =
            nullptr;

        switch (transfer->targetEdge)
        {
        case world::TileEdge::North:
            target = &boundary.north;
            break;
        case world::TileEdge::East:
            target = &boundary.east;
            break;
        case world::TileEdge::South:
            target = &boundary.south;
            break;
        case world::TileEdge::West:
            target = &boundary.west;
            break;
        }

        for (u32 index = 0U;
             index < resolution;
             ++index)
        {
            if (!transfer->cells[index].water)
            {
                throw std::invalid_argument(
                    "M25 coastal boundary source lacks M17 state.");
            }

            (*target)[index] =
                *transfer->cells[index].water;
        }

        haveEdge[edgeIndex] = true;

        revision =
            CombinedRevision(
                revision,
                transfer->source,
                transfer->revision);
    }

    if (!std::all_of(
            haveEdge.begin(),
            haveEdge.end(),
            [](const bool value)
            {
                return value;
            }))
    {
        throw std::invalid_argument(
            "M25 coastal boundary requires all four physical neighbors.");
    }

    boundary.revision = revision;
    return boundary;
}
} // namespace orbit::terrain_region
