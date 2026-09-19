#include <orbit/terrain_debug/TerrainDebugField.hpp>

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M29 failure: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        Fail(message);
    }
}

bool TraceContains(
    const terrain_debug::TerrainDebugField field,
    const terrain_debug::TerrainDebugStage stage)
{
    const auto trace =
        terrain_debug::Descriptor(field).upstream;

    for (const auto value : trace)
    {
        if (value == stage)
        {
            return true;
        }
    }

    return false;
}

terrain_debug::TerrainDebugPageStamp MakePage(
    const world::PlanetTileId tile,
    const u8 lod = 2,
    const u64 processRevision = 9)
{
    return {
        .address = {
            .planet = {
                .high = 0x4f524249544d3239ULL,
                .low = 1
            },
            .tile = tile
        },
        .physicalLod = lod,
        .revisions = {
            .geology = 1,
            .climate = 2,
            .authoring = 3,
            .biome = 4,
            .water = 5,
            .processes = processRevision
        },
        .cacheResident = true,
        .invalidationRevision = 7
    };
}

void TestRequiredFieldCatalog()
{
    const auto catalog =
        terrain_debug::FieldCatalog();

    Require(
        catalog.size() == 21U &&
        catalog.size() ==
            terrain_debug::kRequiredTerrainDebugFieldCount,
        "M29 must expose all 21 required debug fields.");

    std::set<std::string_view> names;

    for (std::size_t i = 0;
         i < catalog.size();
         ++i)
    {
        const auto& entry = catalog[i];

        Require(
            static_cast<std::size_t>(entry.field) == i,
            "Debug field catalog order must match stable enum identity.");

        Require(
            !entry.name.empty() &&
            !entry.upstream.empty(),
            "Every visible debug field must have a name and upstream trace.");

        Require(
            names.insert(entry.name).second,
            "Debug field names must be unique.");

        Require(
            entry.seamRelevant,
            "All M29 required page views must support explicit seam inspection.");
    }
}

void TestTraceability()
{
    using Field = terrain_debug::TerrainDebugField;
    using Stage = terrain_debug::TerrainDebugStage;

    Require(
        TraceContains(Field::BedrockType, Stage::MaterialColumn) &&
        TraceContains(Field::BedrockType, Stage::Geology),
        "Bedrock view must trace through geology into physical material state.");

    Require(
        TraceContains(Field::WaterFlux, Stage::Hydraulic) &&
        TraceContains(Field::WaterFlux, Stage::Drainage),
        "Water flux must trace to drainage and hydraulic state.");

    Require(
        TraceContains(Field::SedimentFlux, Stage::SedimentExchange),
        "Sediment flux must trace to canonical M14 sediment exchange.");

    Require(
        TraceContains(Field::BiomeWeights, Stage::BiomePlacement) &&
        TraceContains(Field::FinalBiome, Stage::BiomeResolution),
        "Biome views must trace to M20 placement and final resolution.");

    Require(
        TraceContains(Field::ScatterDensity, Stage::Scatter) &&
        TraceContains(Field::ScatterDensity, Stage::ExposedSurface),
        "Scatter density must trace through physical surface and deterministic scatter.");

    Require(
        TraceContains(Field::CacheInvalidation, Stage::DependencyInvalidation) &&
        TraceContains(Field::CacheResidency, Stage::PersistentCache),
        "Cache debug views must trace to M27 invalidation and M26 residency.");
}

void TestCubeFaceSeamProbe()
{
    const world::PlanetTileId tile{
        .face = world::CubeFace::PositiveX,
        .level = 4,
        .x = 15,
        .y = 7
    };

    const auto page = MakePage(tile);
    const auto expected =
        terrain_debug::ExpectedNeighbor(
            page.address,
            world::TileEdge::East);

    Require(
        expected.tile.face != tile.face,
        "Test edge must cross a cube face.");

    auto neighbor = MakePage(expected.tile);

    const auto good =
        terrain_debug::ProbeSeam(
            page,
            world::TileEdge::East,
            &neighbor);

    Require(
        good.neighborPresent &&
        good.IsContinuousCandidate(),
        "Matching cross-face physical pages must be seam-comparable.");

    neighbor.physicalLod = 3;

    const auto lodMismatch =
        terrain_debug::ProbeSeam(
            page,
            world::TileEdge::East,
            &neighbor);

    Require(
        lodMismatch.neighborPresent &&
        !lodMismatch.matchingPhysicalLod &&
        !lodMismatch.IsContinuousCandidate(),
        "Physical LOD mismatch must be visible to seam diagnostics.");

    neighbor = MakePage(expected.tile, 2, 10);

    const auto revisionMismatch =
        terrain_debug::ProbeSeam(
            page,
            world::TileEdge::East,
            &neighbor);

    Require(
        !revisionMismatch.matchingRevisions &&
        !revisionMismatch.IsContinuousCandidate(),
        "Revision mismatch must be visible to seam diagnostics.");
}

void TestDebugFingerprint()
{
    const auto page = MakePage({
        .face = world::CubeFace::PositiveY,
        .level = 5,
        .x = 9,
        .y = 11
    });

    const u64 a =
        terrain_debug::DebugPageFingerprint(page);
    const u64 b =
        terrain_debug::DebugPageFingerprint(page);

    Require(
        a != 0 && a == b,
        "Debug page identity must be stable for identical physical provenance.");

    auto changed = page;
    ++changed.invalidationRevision;

    Require(
        a !=
            terrain_debug::DebugPageFingerprint(changed),
        "Invalidation revision must be visible in debug-page provenance.");
}
} // namespace

int main()
{
    TestRequiredFieldCatalog();
    TestTraceability();
    TestCubeFaceSeamProbe();
    TestDebugFingerprint();

    std::cout << "Orbit M29 debug field contract tests passed.\n";
    return EXIT_SUCCESS;
}
