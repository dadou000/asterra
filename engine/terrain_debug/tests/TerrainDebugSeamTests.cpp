#include <orbit/terrain_debug/TerrainDebugPageData.hpp>
#include <orbit/terrain_debug/TerrainDebugSeam.hpp>

#include <cstdlib>
#include <iostream>
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
} // namespace

int main()
{
    TestCrossFaceSampleMapping();
    TestValueMismatchIsReported();

    std::cout << "Orbit M29 value-level seam tests passed.\n";
    return EXIT_SUCCESS;
}
