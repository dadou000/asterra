#include <orbit/terrain_region/SurfaceBoundaryExchange.hpp>

#include <algorithm>
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

void Require(
    const bool condition,
    const std::string& message)
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
        .low = 0x0000000000001000ULL
    };
}

terrain::PhysicalTerrainPageAddress TestPage()
{
    return {
        .planet = TestPlanet(),
        .tile = {
            .face = world::CubeFace::PositiveX,
            .level = 3,
            .x = 3,
            .y = 3
        }
    };
}

terrain_geology::RockTypeId TestRock()
{
    return {
        .high = 0x4F524249544D3235ULL,
        .low = 0x0000000000002000ULL
    };
}

terrain_material_column::MaterialColumnPage
MakeMaterial(const f32 offset)
{
    terrain_material_column::MaterialColumnPage page(
        5,
        2.0);

    for (u32 y = 0U; y < page.Resolution(); ++y)
    {
        for (u32 x = 0U; x < page.Resolution(); ++x)
        {
            const f32 height =
                offset +
                static_cast<f32>(x) +
                static_cast<f32>(y) * 0.1F;

            page.SetCell(
                x,
                y,
                {
                    .bedrockHeightMeters = height,
                    .referenceBedrockHeightMeters = height,
                    .bedrockMaterial = TestRock(),
                    .regolithMeters = 0.0F,
                    .soilMeters = 0.1F,
                    .sandMeters = 0.0F,
                    .debrisMeters = 0.0F,
                    .moisture = 0.2F,
                    .temporaryScalar = 0.0F
                });
        }
    }

    return page;
}

void TestWaterAndSedimentRemapConservesMass()
{
    const auto sourceAddress = TestPage();

    terrain_erosion::SedimentExchangePage sourceSediment(
        5,
        2.0);

    sourceSediment.Add(
        4,
        2,
        terrain_erosion::SedimentTransportMedium::Airborne,
        {
            .sandKg = 7.0,
            .finesKg = 2.0,
            .coarseDebrisKg = 0.0
        });

    sourceSediment.Add(
        4,
        2,
        terrain_erosion::SedimentTransportMedium::SurfaceMobile,
        {
            .sandKg = 3.0,
            .finesKg = 0.0,
            .coarseDebrisKg = 1.0
        });

    const auto exportedAir =
        sourceSediment.ExportAcrossBoundary(
            4,
            2,
            5,
            2,
            terrain_erosion::SedimentTransportMedium::Airborne,
            {
                .sandKg = 7.0,
                .finesKg = 2.0,
                .coarseDebrisKg = 0.0
            });

    const auto exportedSurface =
        sourceSediment.ExportAcrossBoundary(
            4,
            2,
            5,
            2,
            terrain_erosion::SedimentTransportMedium::SurfaceMobile,
            {
                .sandKg = 3.0,
                .finesKg = 0.0,
                .coarseDebrisKg = 1.0
            });

    RequireNear(
        exportedAir.TotalKg() +
            exportedSurface.TotalKg(),
        13.0,
        1.0e-12,
        "M14 must export the requested dune/saltation mass.");

    auto sedimentFlux =
        sourceSediment.TakeOutgoingBoundaryFlux(77);

    auto flux =
        terrain_region::MakeSurfaceBoundaryFlux(
            5,
            77,
            std::move(sedimentFlux));

    flux.east[2] = {
        .volumeCubicMeters = 12.5,
        .velocityMoment = {25.0, 5.0}
    };

    const terrain_region::PhysicalPageBoundaryFlux source{
        .address = sourceAddress,
        .outgoing = std::move(flux)
    };

    const auto batch =
        terrain_region::BuildDeterministicBoundaryTransfers(
            std::span<const terrain_region::PhysicalPageBoundaryFlux>(
                &source,
                1));

    RequireNear(
        batch.TotalWaterVolumeCubicMeters(),
        12.5,
        1.0e-12,
        "Boundary remapping must preserve water volume.");

    RequireNear(
        batch.TotalSediment().TotalKg(),
        13.0,
        1.0e-12,
        "Boundary remapping must preserve aeolian/surface sediment mass.");

    const auto mapping =
        world::NeighborAcrossTileEdge(
            sourceAddress.tile,
            world::TileEdge::East);

    const terrain::PhysicalTerrainPageAddress receiver{
        .planet = sourceAddress.planet,
        .tile = mapping.tile
    };

    terrain_erosion::SedimentExchangePage targetSediment(
        5,
        2.0);

    terrain_region::ApplySedimentBoundaryTransfers(
        receiver,
        batch,
        targetSediment);

    RequireNear(
        targetSediment.TotalMobileMass().TotalKg(),
        13.0,
        1.0e-12,
        "Dune/saltation mass must arrive in the neighboring physical page.");

    const auto incomingWater =
        terrain_region::IncomingWaterBoundaryFlux(
            receiver,
            batch);

    RequireNear(
        incomingWater.volumeCubicMeters,
        12.5,
        1.0e-12,
        "Receiver must observe exactly the exported water volume.");

    RequireNear(
        sourceSediment.TotalMobileMass().TotalKg() +
            targetSediment.TotalMobileMass().TotalKg(),
        13.0,
        1.0e-12,
        "Two-page mobile sediment mass must be conserved.");
}

void TestCrossFaceVectorAndSampleOrientation()
{
    terrain::PhysicalTerrainPageAddress source{
        .planet = TestPlanet(),
        .tile = {
            .face = world::CubeFace::PositiveX,
            .level = 2,
            .x = 3,
            .y = 1
        }
    };

    const auto mapping =
        world::NeighborAcrossTileEdge(
            source.tile,
            world::TileEdge::East);

    Require(
        mapping.tile.face != source.tile.face,
        "Test tile must cross a cube face.");

    auto flux =
        terrain_region::MakeSurfaceBoundaryFlux(
            4,
            91);

    for (u32 index = 0U; index < 4U; ++index)
    {
        flux.east[index] = {
            .volumeCubicMeters =
                static_cast<f64>(index + 1U),
            .velocityMoment = {
                static_cast<f64>(index + 1U),
                0.25
            }
        };
    }

    const terrain_region::PhysicalPageBoundaryFlux page{
        .address = source,
        .outgoing = std::move(flux)
    };

    const auto batch =
        terrain_region::BuildDeterministicBoundaryTransfers(
            std::span<const terrain_region::PhysicalPageBoundaryFlux>(
                &page,
                1));

    const auto it =
        std::find_if(
            batch.edges.begin(),
            batch.edges.end(),
            [&](const auto& transfer)
            {
                return
                    transfer.sourceEdge ==
                        world::TileEdge::East;
            });

    Require(
        it != batch.edges.end(),
        "Cross-face east transfer must be present.");

    Require(
        it->target.tile == mapping.tile &&
        it->targetEdge == mapping.edge &&
        it->reversed == mapping.reverseSamples,
        "M25 must use the canonical cube-face neighbor mapping.");

    for (u32 sourceIndex = 0U;
         sourceIndex < 4U;
         ++sourceIndex)
    {
        const u32 targetIndex =
            world::RemapTileEdgeSampleIndex(
                mapping,
                sourceIndex,
                4);

        RequireNear(
            it->water[targetIndex].volumeCubicMeters,
            static_cast<f64>(sourceIndex + 1U),
            0.0,
            "Cross-face sample order must be canonical.");

        Require(
            std::isfinite(
                it->water[targetIndex].
                    velocityMoment.x) &&
            std::isfinite(
                it->water[targetIndex].
                    velocityMoment.y),
            "Cross-face vector remap must stay finite.");
    }
}

void TestGhostSnapshotsAreDeterministic()
{
    const auto aAddress = TestPage();

    const auto east =
        world::NeighborAcrossTileEdge(
            aAddress.tile,
            world::TileEdge::East);

    const terrain::PhysicalTerrainPageAddress bAddress{
        .planet = aAddress.planet,
        .tile = east.tile
    };

    const auto aMaterial =
        MakeMaterial(100.0F);
    const auto bMaterial =
        MakeMaterial(200.0F);

    const auto a =
        terrain_region::BuildSurfaceBoundarySnapshot(
            aAddress,
            aMaterial,
            nullptr,
            nullptr,
            10);

    const auto b =
        terrain_region::BuildSurfaceBoundarySnapshot(
            bAddress,
            bMaterial,
            nullptr,
            nullptr,
            20);

    const std::array<
        terrain_region::SurfaceBoundarySnapshot,
        2> forward{a, b};

    const std::array<
        terrain_region::SurfaceBoundarySnapshot,
        2> reverse{b, a};

    const auto x =
        terrain_region::BuildDeterministicGhostTransfers(
            forward);

    const auto y =
        terrain_region::BuildDeterministicGhostTransfers(
            reverse);

    Require(
        x.edges.size() == y.edges.size() &&
        x.corners.size() == y.corners.size(),
        "Ghost exchange result size must not depend on input order.");

    for (std::size_t index = 0U;
         index < x.edges.size();
         ++index)
    {
        Require(
            x.edges[index].source ==
                y.edges[index].source &&
            x.edges[index].target ==
                y.edges[index].target &&
            x.edges[index].targetEdge ==
                y.edges[index].targetEdge,
            "Ghost synchronization ordering must be deterministic.");
    }

    const auto incoming =
        std::find_if(
            x.edges.begin(),
            x.edges.end(),
            [&](const auto& transfer)
            {
                return
                    transfer.source == aAddress &&
                    transfer.target == bAddress;
            });

    Require(
        incoming != x.edges.end(),
        "Adjacent physical pages must exchange a ghost edge.");

    for (u32 sourceIndex = 0U;
         sourceIndex < a.resolution;
         ++sourceIndex)
    {
        const u32 targetIndex =
            world::RemapTileEdgeSampleIndex(
                east,
                sourceIndex,
                a.resolution);

        RequireNear(
            incoming->cells[targetIndex].
                material.SurfaceHeightMeters(),
            aMaterial.At(
                aMaterial.Resolution() - 1U,
                sourceIndex).
                    SurfaceHeightMeters(),
            1.0e-6,
            "Physical M08 ghost edge must match source boundary state.");
    }
}

void TestBoundaryVectorMapsOutwardToReceiverInward()
{
    const auto source = TestPage();

    for (u8 raw = 0U; raw < 4U; ++raw)
    {
        const auto edge =
            static_cast<world::TileEdge>(raw);

        const auto mapping =
            world::NeighborAcrossTileEdge(
                source.tile,
                edge);

        math::Double2 outward{};

        switch (edge)
        {
        case world::TileEdge::North:
            outward = {0.0, -1.0};
            break;
        case world::TileEdge::East:
            outward = {1.0, 0.0};
            break;
        case world::TileEdge::South:
            outward = {0.0, 1.0};
            break;
        case world::TileEdge::West:
            outward = {-1.0, 0.0};
            break;
        }

        const auto transformed =
            terrain_region::TransformBoundaryVectorAcrossEdge(
                edge,
                mapping,
                outward);

        math::Double2 expectedInward{};

        switch (mapping.edge)
        {
        case world::TileEdge::North:
            expectedInward = {0.0, 1.0};
            break;
        case world::TileEdge::East:
            expectedInward = {-1.0, 0.0};
            break;
        case world::TileEdge::South:
            expectedInward = {0.0, -1.0};
            break;
        case world::TileEdge::West:
            expectedInward = {1.0, 0.0};
            break;
        }

        RequireNear(
            transformed.x,
            expectedInward.x,
            1.0e-12,
            "Outgoing page-normal flux must become receiver-inward flux.");
        RequireNear(
            transformed.y,
            expectedInward.y,
            1.0e-12,
            "Outgoing page-normal flux must become receiver-inward flux.");
    }
}
} // namespace

int main()
{
    TestWaterAndSedimentRemapConservesMass();
    TestCrossFaceVectorAndSampleOrientation();
    TestGhostSnapshotsAreDeterministic();
    TestBoundaryVectorMapsOutwardToReceiverInward();

    std::cout << "Orbit M25 physical boundary exchange tests passed.\n";
    return EXIT_SUCCESS;
}
