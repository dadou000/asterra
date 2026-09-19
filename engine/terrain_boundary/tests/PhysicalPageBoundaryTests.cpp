#include <orbit/terrain_boundary/PhysicalPageBoundary.hpp>

#include <orbit/terrain_erosion/SedimentExchange.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M25 failure: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        Fail(message);
    }
}

void RequireNear(
    const f64 a,
    const f64 b,
    const f64 tolerance,
    const std::string& message)
{
    if (std::abs(a - b) > tolerance)
    {
        Fail(message);
    }
}

world::PlanetId TestPlanet()
{
    return {
        .high = 0x4F524249544D3235ULL,
        .low = 0x0000000000000001ULL
    };
}

terrain_geology::RockTypeId TestRock()
{
    return {
        .high = 0x4F524249544D3235ULL,
        .low = 0x0000000000000010ULL
    };
}

terrain::PhysicalTerrainPageKey MakeKey(
    const world::PlanetTileId tile)
{
    return {
        .address = {
            .planet = TestPlanet(),
            .tile = tile
        },
        .resolution = 5,
        .revisions = {
            .geology = 1,
            .climate = 2,
            .authoring = 3,
            .biome = 4,
            .water = 5,
            .processes = 6
        }
    };
}

terrain_material_column::MaterialColumnPage MakeMaterial()
{
    terrain_material_column::MaterialColumnPage page(
        5,
        10.0);

    for (u32 y = 0; y < 5; ++y)
    {
        for (u32 x = 0; x < 5; ++x)
        {
            const f32 height =
                100.0F +
                static_cast<f32>(x) +
                static_cast<f32>(y) * 0.1F;

            page.SetCell(
                x,
                y,
                {
                    .bedrockHeightMeters = height,
                    .referenceBedrockHeightMeters = height,
                    .bedrockMaterial = TestRock(),
                    .regolithMeters = 0.2F,
                    .soilMeters = 0.3F,
                    .sandMeters = 0.1F,
                    .debrisMeters = 0.0F,
                    .moisture = 0.4F,
                    .temporaryScalar = 0.0F
                });
        }
    }

    return page;
}

terrain_erosion::SedimentBoundaryFlux MakeSedimentFlux()
{
    terrain_erosion::SedimentBoundaryFlux flux{
        .north =
            std::vector<
                terrain_erosion::SedimentTransportPacket>(5),
        .east =
            std::vector<
                terrain_erosion::SedimentTransportPacket>(5),
        .south =
            std::vector<
                terrain_erosion::SedimentTransportPacket>(5),
        .west =
            std::vector<
                terrain_erosion::SedimentTransportPacket>(5),
        .revision = 9
    };

    for (u32 i = 0; i < 5; ++i)
    {
        flux.east[i].waterborne.sandKg =
            1.0 + static_cast<f64>(i);
        flux.east[i].airborne.finesKg =
            0.25 * static_cast<f64>(i + 1U);
        flux.east[i].surfaceMobile.coarseDebrisKg =
            0.5;
    }

    return flux;
}

std::vector<terrain_boundary::WaterBoundaryFlux>
MakeWaterFlux()
{
    std::vector<terrain_boundary::WaterBoundaryFlux> water(5);
    for (u32 i = 0; i < 5; ++i)
    {
        water[i] = {
            .volumeCubicMeters =
                10.0 + static_cast<f64>(i),
            .dischargeCubicMetersPerSecond =
                2.0 + static_cast<f64>(i) * 0.1
        };
    }
    return water;
}

void TestSameFaceExchangeConservesTransport()
{
    const world::PlanetTileId sourceTile{
        .face = world::CubeFace::PositiveX,
        .level = 3,
        .x = 3,
        .y = 2
    };

    const auto mapping =
        world::NeighborAcrossTileEdge(
            sourceTile,
            world::TileEdge::East);

    Require(
        mapping.tile.face ==
            world::CubeFace::PositiveX,
        "Selected same-face test edge must remain on one cube face.");

    const auto sourceKey = MakeKey(sourceTile);
    const auto destinationKey = MakeKey(mapping.tile);
    const auto material = MakeMaterial();
    const auto sedimentFlux = MakeSedimentFlux();
    const auto water = MakeWaterFlux();

    const auto outgoing =
        terrain_boundary::BuildPhysicalPageEdgeState(
            sourceKey,
            material,
            nullptr,
            world::TileEdge::East,
            water,
            &sedimentFlux,
            77);

    const auto incoming =
        terrain_boundary::RemapPhysicalPageEdge(
            outgoing,
            destinationKey);

    const auto before =
        terrain_boundary::TransportTotals(outgoing);
    const auto after =
        terrain_boundary::TransportTotals(incoming);

    RequireNear(
        before.waterCubicMeters,
        after.waterCubicMeters,
        0.0,
        "Water volume must be exactly conserved by edge remap.");

    RequireNear(
        before.sediment.TotalKg(),
        after.sediment.TotalKg(),
        0.0,
        "Sediment mass must be exactly conserved by edge remap.");

    Require(
        incoming.receivingEdge == mapping.edge,
        "Incoming edge must use the geometric receiving side.");
}

void TestCubeFaceExchangeUsesGeometricSampleRemap()
{
    const world::PlanetTileId sourceTile{
        .face = world::CubeFace::PositiveX,
        .level = 2,
        .x = 3,
        .y = 1
    };

    const auto mapping =
        world::NeighborAcrossTileEdge(
            sourceTile,
            world::TileEdge::East);

    Require(
        mapping.tile.face != sourceTile.face,
        "Test tile must cross a cube-face boundary.");

    const auto sourceKey = MakeKey(sourceTile);
    const auto destinationKey = MakeKey(mapping.tile);
    const auto material = MakeMaterial();
    const auto sedimentFlux = MakeSedimentFlux();
    const auto water = MakeWaterFlux();

    const auto outgoing =
        terrain_boundary::BuildPhysicalPageEdgeState(
            sourceKey,
            material,
            nullptr,
            world::TileEdge::East,
            water,
            &sedimentFlux,
            88);

    const auto incoming =
        terrain_boundary::RemapPhysicalPageEdge(
            outgoing,
            destinationKey);

    for (u32 sourceIndex = 0; sourceIndex < 5; ++sourceIndex)
    {
        const u32 destinationIndex =
            world::RemapTileEdgeSampleIndex(
                mapping,
                sourceIndex,
                5);

        RequireNear(
            incoming.samples[destinationIndex].
                water.volumeCubicMeters,
            outgoing.samples[sourceIndex].
                water.volumeCubicMeters,
            0.0,
            "Cross-face remap must preserve each water packet.");

        RequireNear(
            incoming.samples[destinationIndex].
                sediment.Total().TotalKg(),
            outgoing.samples[sourceIndex].
                sediment.Total().TotalKg(),
            0.0,
            "Cross-face remap must preserve each sediment packet.");
    }
}

void TestHaloAssemblyIsDeterministic()
{
    terrain_boundary::PhysicalPageHalo halo;
    const auto material = MakeMaterial();

    const world::PlanetTileId center{
        .face = world::CubeFace::PositiveY,
        .level = 3,
        .x = 4,
        .y = 4
    };

    const auto centerKey = MakeKey(center);

    for (const world::TileEdge receiving :
         {world::TileEdge::North,
          world::TileEdge::East,
          world::TileEdge::South,
          world::TileEdge::West})
    {
        const auto neighbor =
            world::NeighborAcrossTileEdge(
                center,
                receiving);

        const auto back =
            world::NeighborAcrossTileEdge(
                neighbor.tile,
                neighbor.edge);

        Require(
            back.tile == center,
            "Neighbor mapping must be reversible for halo construction.");

        const auto neighborKey =
            MakeKey(neighbor.tile);

        const auto outgoing =
            terrain_boundary::BuildPhysicalPageEdgeState(
                neighborKey,
                material,
                nullptr,
                neighbor.edge,
                {},
                nullptr,
                100 +
                    static_cast<u64>(receiving));

        const auto incoming =
            terrain_boundary::RemapPhysicalPageEdge(
                outgoing,
                centerKey);

        halo.SetEdge(incoming);
    }

    Require(
        halo.IsComplete(5),
        "Four deterministic neighbor edges must form a complete halo.");
}

void TestM14ImportPreservesTypedMassAndAccounting()
{
    const world::PlanetTileId sourceTile{
        .face = world::CubeFace::PositiveX,
        .level = 3,
        .x = 3,
        .y = 2
    };

    const auto mapping =
        world::NeighborAcrossTileEdge(
            sourceTile,
            world::TileEdge::East);

    const auto sourceKey = MakeKey(sourceTile);
    const auto destinationKey = MakeKey(mapping.tile);
    const auto material = MakeMaterial();
    const auto sedimentFlux = MakeSedimentFlux();

    const auto outgoing =
        terrain_boundary::BuildPhysicalPageEdgeState(
            sourceKey,
            material,
            nullptr,
            world::TileEdge::East,
            {},
            &sedimentFlux,
            123);

    const auto incoming =
        terrain_boundary::RemapPhysicalPageEdge(
            outgoing,
            destinationKey);

    terrain_erosion::SedimentExchangePage destination(
        5,
        10.0);

    terrain_boundary::ImportSedimentEdge(
        incoming,
        destination);

    RequireNear(
        destination.TotalMobileMass().TotalKg(),
        incoming.TotalSediment().TotalKg(),
        1.0e-12,
        "Imported M14 mass must equal remapped boundary mass.");

    RequireNear(
        destination.Accounting().imported.TotalKg(),
        incoming.TotalSediment().TotalKg(),
        1.0e-12,
        "M25 import must preserve M14 imported-mass accounting.");
}

void TestExchangeFingerprintIgnoresViewState()
{
    const world::PlanetTileId sourceTile{
        .face = world::CubeFace::PositiveZ,
        .level = 4,
        .x = 6,
        .y = 7
    };

    const auto mapping =
        world::NeighborAcrossTileEdge(
            sourceTile,
            world::TileEdge::South);

    const auto sourceKey = MakeKey(sourceTile);
    const auto destinationKey = MakeKey(mapping.tile);

    const u64 a =
        terrain_boundary::BoundaryExchangeFingerprint(
            sourceKey,
            destinationKey,
            world::TileEdge::South,
            55);

    const u64 b =
        terrain_boundary::BoundaryExchangeFingerprint(
            sourceKey,
            destinationKey,
            world::TileEdge::South,
            55);

    Require(
        a != 0 && a == b,
        "Physical/revision-identical boundary exchange must be stable.");

    Require(
        a != terrain_boundary::BoundaryExchangeFingerprint(
            sourceKey,
            destinationKey,
            world::TileEdge::South,
            56),
        "Boundary physical revision must invalidate exchange identity.");
}
} // namespace

int main()
{
    TestSameFaceExchangeConservesTransport();
    TestCubeFaceExchangeUsesGeometricSampleRemap();
    TestHaloAssemblyIsDeterministic();
    TestM14ImportPreservesTypedMassAndAccounting();
    TestExchangeFingerprintIgnoresViewState();

    std::cout << "Orbit M25 boundary exchange tests passed.\n";
    return EXIT_SUCCESS;
}
