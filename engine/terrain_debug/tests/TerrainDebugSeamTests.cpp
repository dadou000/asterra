#include <orbit/terrain_debug/TerrainDebugLivePages.hpp>
#include <orbit/terrain_debug/TerrainDebugPageData.hpp>
#include <orbit/terrain_debug/TerrainDebugSeam.hpp>
#include <orbit/terrain_region/SurfaceBoundaryExchange.hpp>

#include <cstdlib>
#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M29 seam failure: "
              << message << '\n';
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

terrain_debug::TerrainDebugPageStamp MakeStamp(
    const world::PlanetTileId tile)
{
    return {
        .address = {
            .planet = {
                .high = 0x4d32395345414d31ULL,
                .low = 1
            },
            .tile = tile
        },
        .physicalLod = 3,
        .revisions = {
            .geology = 1,
            .climate = 2,
            .authoring = 3,
            .biome = 4,
            .water = 5,
            .processes = 6
        },
        .cacheResident = true,
        .invalidationRevision = 9
    };
}

void TestCrossFaceSampleMapping()
{
    const world::PlanetTileId tile{
        .face = world::CubeFace::PositiveX,
        .level = 3,
        .x = 7,
        .y = 3
    };

    const auto sourceStamp =
        MakeStamp(tile);
    const auto mapping =
        world::NeighborAcrossTileEdge(
            tile,
            world::TileEdge::East);
    const auto neighborStamp =
        MakeStamp(mapping.tile);

    terrain_debug::TerrainDebugPageData source(
        sourceStamp,
        4,
        4);
    terrain_debug::TerrainDebugPageData neighbor(
        neighborStamp,
        4,
        4);

    std::vector<f32> sourceValues(16, 0.0F);
    std::vector<f32> neighborValues(16, 0.0F);

    for (u32 i = 0; i < 4; ++i)
    {
        const f32 value =
            static_cast<f32>(10U + i);

        sourceValues[
            static_cast<std::size_t>(i) * 4U +
            3U] = value;

        const u32 j =
            world::RemapTileEdgeSampleIndex(
                mapping,
                i,
                4);

        std::size_t neighborIndex = 0;
        switch (mapping.edge)
        {
        case world::TileEdge::North:
            neighborIndex = j;
            break;
        case world::TileEdge::East:
            neighborIndex =
                static_cast<std::size_t>(j) * 4U +
                3U;
            break;
        case world::TileEdge::South:
            neighborIndex =
                12U + j;
            break;
        case world::TileEdge::West:
            neighborIndex =
                static_cast<std::size_t>(j) * 4U;
            break;
        }

        neighborValues[neighborIndex] = value;
    }

    source.SetScalar(
        terrain_debug::TerrainDebugField::Soil,
        sourceValues);
    neighbor.SetScalar(
        terrain_debug::TerrainDebugField::Soil,
        neighborValues);

    const auto sourceView =
        source.View(
            terrain_debug::TerrainDebugField::Soil);
    const auto neighborView =
        neighbor.View(
            terrain_debug::TerrainDebugField::Soil);

    const auto continuous =
        terrain_debug::CompareSeamValues(
            sourceStamp,
            sourceView,
            world::TileEdge::East,
            &neighborStamp,
            &neighborView);

    Require(
        continuous.Comparable() &&
        continuous.ValuesContinuous() &&
        continuous.samplesCompared == 4U,
        "Cross-face seam comparison must honor receiving edge and sample orientation.");
}

void TestCrossFaceVectorOrientation()
{
    const world::PlanetTileId tile{
        .face = world::CubeFace::PositiveX,
        .level = 3,
        .x = 7,
        .y = 1
    };

    const auto sourceStamp = MakeStamp(tile);
    const auto mapping =
        world::NeighborAcrossTileEdge(
            tile,
            world::TileEdge::East);
    const auto neighborStamp =
        MakeStamp(mapping.tile);

    terrain_debug::TerrainDebugPageData source(
        sourceStamp, 2, 2);
    terrain_debug::TerrainDebugPageData neighbor(
        neighborStamp, 2, 2);

    std::vector<terrain_debug::TerrainDebugVector2>
        sourceValues(4);
    std::vector<terrain_debug::TerrainDebugVector2>
        neighborValues(4);

    for (u32 i = 0; i < 2; ++i)
    {
        const terrain_debug::TerrainDebugVector2 value{
            .x = 2.0F + static_cast<f32>(i),
            .y = -1.0F
        };
        sourceValues[
            static_cast<std::size_t>(i) * 2U + 1U] =
                value;

        const auto transformed =
            terrain_region::
                TransformBoundaryVectorAcrossEdge(
                    world::TileEdge::East,
                    mapping,
                    {
                        static_cast<f64>(value.x),
                        static_cast<f64>(value.y)
                    });

        const terrain_debug::TerrainDebugVector2 target{
            .x = static_cast<f32>(transformed.x),
            .y = static_cast<f32>(transformed.y)
        };
        const u32 j =
            world::RemapTileEdgeSampleIndex(
                mapping, i, 2);

        switch (mapping.edge)
        {
        case world::TileEdge::North:
            neighborValues[j] = target;
            break;
        case world::TileEdge::East:
            neighborValues[
                static_cast<std::size_t>(j) * 2U + 1U] =
                    target;
            break;
        case world::TileEdge::South:
            neighborValues[2U + j] = target;
            break;
        case world::TileEdge::West:
            neighborValues[
                static_cast<std::size_t>(j) * 2U] =
                    target;
            break;
        }
    }

    source.SetVector(
        terrain_debug::TerrainDebugField::Drainage,
        sourceValues);
    neighbor.SetVector(
        terrain_debug::TerrainDebugField::Drainage,
        neighborValues);

    const auto sourceView =
        source.View(
            terrain_debug::TerrainDebugField::Drainage);
    const auto neighborView =
        neighbor.View(
            terrain_debug::TerrainDebugField::Drainage);

    const auto comparison =
        terrain_debug::CompareSeamValues(
            sourceStamp,
            sourceView,
            world::TileEdge::East,
            &neighborStamp,
            &neighborView,
            1.0e-5);

    Require(
        comparison.ValuesContinuous(),
        "Cross-face vector seams must compare in the receiving page tangent frame.");
}

void TestValueMismatchIsReported()
{
    const world::PlanetTileId tile{
        .face = world::CubeFace::PositiveZ,
        .level = 3,
        .x = 2,
        .y = 2
    };

    const auto sourceStamp =
        MakeStamp(tile);
    const auto mapping =
        world::NeighborAcrossTileEdge(
            tile,
            world::TileEdge::East);
    const auto neighborStamp =
        MakeStamp(mapping.tile);

    terrain_debug::TerrainDebugPageData source(
        sourceStamp,
        2,
        2);
    terrain_debug::TerrainDebugPageData neighbor(
        neighborStamp,
        2,
        2);

    const std::vector<f32> sourceValues{
        0.0F, 1.0F,
        0.0F, 2.0F};
    std::vector<f32> neighborValues{
        0.0F, 0.0F,
        0.0F, 0.0F};

    for (u32 i = 0; i < 2; ++i)
    {
        const u32 j =
            world::RemapTileEdgeSampleIndex(
                mapping,
                i,
                2);
        const f32 value =
            sourceValues[
                static_cast<std::size_t>(i) * 2U +
                1U];

        switch (mapping.edge)
        {
        case world::TileEdge::North:
            neighborValues[j] = value;
            break;
        case world::TileEdge::East:
            neighborValues[
                static_cast<std::size_t>(j) * 2U +
                1U] = value;
            break;
        case world::TileEdge::South:
            neighborValues[2U + j] = value;
            break;
        case world::TileEdge::West:
            neighborValues[
                static_cast<std::size_t>(j) * 2U] = value;
            break;
        }
    }

    // Corrupt one receiving edge sample after canonical mapping.
    const u32 corruptJ =
        world::RemapTileEdgeSampleIndex(
            mapping,
            1,
            2);
    switch (mapping.edge)
    {
    case world::TileEdge::North:
        neighborValues[corruptJ] += 0.5F;
        break;
    case world::TileEdge::East:
        neighborValues[
            static_cast<std::size_t>(corruptJ) * 2U +
            1U] += 0.5F;
        break;
    case world::TileEdge::South:
        neighborValues[2U + corruptJ] += 0.5F;
        break;
    case world::TileEdge::West:
        neighborValues[
            static_cast<std::size_t>(corruptJ) * 2U] += 0.5F;
        break;
    }

    source.SetScalar(
        terrain_debug::TerrainDebugField::Soil,
        sourceValues);
    neighbor.SetScalar(
        terrain_debug::TerrainDebugField::Soil,
        neighborValues);

    const auto sourceView =
        source.View(
            terrain_debug::TerrainDebugField::Soil);
    const auto neighborView =
        neighbor.View(
            terrain_debug::TerrainDebugField::Soil);

    const auto mismatch =
        terrain_debug::CompareSeamValues(
            sourceStamp,
            sourceView,
            world::TileEdge::East,
            &neighborStamp,
            &neighborView,
            0.01);

    Require(
        mismatch.Comparable() &&
        !mismatch.ValuesContinuous() &&
        mismatch.mismatchedSamples == 1U &&
        mismatch.maximumDifference > 0.49,
        "A real edge-value discontinuity must be counted and quantified.");
}

std::shared_ptr<terrain_debug::TerrainDebugPageData>
MakeUniformSoilPage(
    const terrain_debug::TerrainDebugPageStamp& stamp,
    const f32 value,
    const bool publishSoil = true)
{
    auto page =
        std::make_shared<
            terrain_debug::TerrainDebugPageData>(
                stamp,
                4,
                4);

    if (publishSoil)
    {
        const std::vector<f32> values(
            16,
            value);

        page->SetScalar(
            terrain_debug::TerrainDebugField::Soil,
            values);
    }

    return page;
}

void TestLiveSeamInspectionClassifiesAllStates()
{
    const world::PlanetTileId tile{
        .face = world::CubeFace::PositiveZ,
        .level = 3,
        .x = 3,
        .y = 3
    };

    const auto sourceStamp =
        MakeStamp(tile);
    const auto source =
        MakeUniformSoilPage(
            sourceStamp,
            1.0F);

    terrain_debug::TerrainDebugLivePages live;

    auto inspections =
        terrain_debug::InspectTerrainDebugSeams(
            *source,
            terrain_debug::TerrainDebugField::Soil,
            live);

    for (const auto& seam : inspections)
    {
        Require(
            seam.state ==
                terrain_debug::
                    TerrainDebugSeamState::
                        MissingNeighbor,
            "Unpublished physical neighbors must be reported as missing.");
    }

    const auto northAddress =
        terrain_debug::ExpectedNeighbor(
            sourceStamp.address,
            world::TileEdge::North);
    auto northStamp =
        MakeStamp(
            northAddress.tile);
    live.Publish(
        MakeUniformSoilPage(
            northStamp,
            1.0F));

    const auto eastAddress =
        terrain_debug::ExpectedNeighbor(
            sourceStamp.address,
            world::TileEdge::East);
    auto eastStamp =
        MakeStamp(
            eastAddress.tile);
    eastStamp.physicalLod =
        static_cast<u8>(
            sourceStamp.physicalLod + 1U);
    live.Publish(
        MakeUniformSoilPage(
            eastStamp,
            1.0F));

    const auto southAddress =
        terrain_debug::ExpectedNeighbor(
            sourceStamp.address,
            world::TileEdge::South);
    auto southStamp =
        MakeStamp(
            southAddress.tile);
    ++southStamp.revisions.geology;
    live.Publish(
        MakeUniformSoilPage(
            southStamp,
            1.0F));

    const auto westAddress =
        terrain_debug::ExpectedNeighbor(
            sourceStamp.address,
            world::TileEdge::West);
    auto westStamp =
        MakeStamp(
            westAddress.tile);
    live.Publish(
        MakeUniformSoilPage(
            westStamp,
            0.0F,
            false));

    inspections =
        terrain_debug::InspectTerrainDebugSeams(
            *source,
            terrain_debug::TerrainDebugField::Soil,
            live);

    Require(
        inspections[
            static_cast<u8>(
                world::TileEdge::North)].
            state ==
            terrain_debug::
                TerrainDebugSeamState::
                    Continuous,
        "Matching live north neighbor must be continuous.");

    Require(
        inspections[
            static_cast<u8>(
                world::TileEdge::East)].
            state ==
            terrain_debug::
                TerrainDebugSeamState::
                    PhysicalLodMismatch,
        "Physical LOD disagreement must be classified explicitly.");

    Require(
        inspections[
            static_cast<u8>(
                world::TileEdge::South)].
            state ==
            terrain_debug::
                TerrainDebugSeamState::
                    RevisionMismatch,
        "Generation revision disagreement must be classified explicitly.");

    Require(
        inspections[
            static_cast<u8>(
                world::TileEdge::West)].
            state ==
            terrain_debug::
                TerrainDebugSeamState::
                    FieldUnavailable,
        "A present neighbor without the selected field must stay unavailable.");

    live.Publish(
        MakeUniformSoilPage(
            westStamp,
            2.0F));

    inspections =
        terrain_debug::InspectTerrainDebugSeams(
            *source,
            terrain_debug::TerrainDebugField::Soil,
            live);

    const auto& west =
        inspections[
            static_cast<u8>(
                world::TileEdge::West)];

    Require(
        west.state ==
            terrain_debug::
                TerrainDebugSeamState::
                    ValueMismatch &&
        west.comparison.mismatchedSamples == 4U &&
        west.comparison.maximumDifference > 0.99,
        "A live field discontinuity must become an explicit value mismatch.");
}

void TestSeamOverlayDrawsCanonicalEdges()
{
    std::vector<u8> rgba(
        4U * 4U * 4U,
        0U);

    const std::array<
        terrain_debug::TerrainDebugSeamInspection,
        4> seams{{
        {
            .edge = world::TileEdge::North,
            .state =
                terrain_debug::
                    TerrainDebugSeamState::
                        Continuous
        },
        {
            .edge = world::TileEdge::East,
            .state =
                terrain_debug::
                    TerrainDebugSeamState::
                        MissingNeighbor
        },
        {
            .edge = world::TileEdge::South,
            .state =
                terrain_debug::
                    TerrainDebugSeamState::
                        PhysicalLodMismatch
        },
        {
            .edge = world::TileEdge::West,
            .state =
                terrain_debug::
                    TerrainDebugSeamState::
                        ValueMismatch
        }
    }};

    terrain_debug::
        ApplyTerrainDebugSeamOverlayRgba8(
            rgba,
            4,
            4,
            seams,
            1);

    const auto pixel =
        [&](const u32 x,
            const u32 y)
        {
            const std::size_t index =
                (static_cast<std::size_t>(y) *
                     4U +
                 x) *
                4U;

            return std::array<u8, 4>{
                rgba[index + 0U],
                rgba[index + 1U],
                rgba[index + 2U],
                rgba[index + 3U]
            };
        };

    Require(
        pixel(1U, 0U) ==
            std::array<u8, 4>{
                48U, 208U, 96U, 255U},
        "North continuous seam must draw the green top border.");

    Require(
        pixel(3U, 1U) ==
            std::array<u8, 4>{
                96U, 96U, 96U, 255U},
        "East missing seam must draw the gray right border.");

    Require(
        pixel(1U, 3U) ==
            std::array<u8, 4>{
                64U, 160U, 255U, 255U},
        "South LOD mismatch must draw the blue bottom border.");

    Require(
        pixel(0U, 1U) ==
            std::array<u8, 4>{
                255U, 64U, 64U, 255U},
        "West value mismatch must draw the red left border.");
}

} // namespace

int main()
{
    TestCrossFaceSampleMapping();
    TestCrossFaceVectorOrientation();
    TestValueMismatchIsReported();
    TestLiveSeamInspectionClassifiesAllStates();
    TestSeamOverlayDrawsCanonicalEdges();

    std::cout << "Orbit M29 value-level seam tests passed.\n";
    return EXIT_SUCCESS;
}
