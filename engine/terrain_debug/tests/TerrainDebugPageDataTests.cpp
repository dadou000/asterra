#include <orbit/terrain_debug/TerrainDebugPageData.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M29 page binding failure: "
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

terrain_debug::TerrainDebugPageStamp MakeStamp()
{
    return {
        .address = {
            .planet = {
                .high = 0x4f524249544d3239ULL,
                .low = 0x5041474544415441ULL
            },
            .tile = {
                .face = world::CubeFace::PositiveZ,
                .level = 4,
                .x = 3,
                .y = 7
            }
        },
        .physicalLod = 2,
        .revisions = {
            .geology = 1,
            .climate = 2,
            .authoring = 3,
            .biome = 4,
            .water = 5,
            .processes = 6
        },
        .cacheResident = true,
        .invalidationRevision = 11
    };
}

void TestMaterialColumnBinding()
{
    terrain_material_column::MaterialColumnPage page(
        2,
        10.0);

    for (u32 y = 0; y < 2; ++y)
    {
        for (u32 x = 0; x < 2; ++x)
        {
            terrain_material_column::MaterialColumnCell cell{};
            cell.bedrockHeightMeters = 10.0F;
            cell.referenceBedrockHeightMeters = 10.0F;
            cell.bedrockMaterial = {
                .high = 0x10U + x,
                .low = 0x20U + y
            };
            cell.regolithMeters = 0.25F;
            cell.soilMeters = 0.5F +
                static_cast<f32>(x);
            cell.sandMeters = 0.125F;
            cell.debrisMeters = 0.0F;
            cell.moisture = 0.3F +
                static_cast<f32>(y) * 0.1F;
            page.SetCell(x, y, cell);
        }
    }

    terrain_debug::TerrainDebugPageData debug(
        MakeStamp(),
        2,
        2);
    debug.CaptureMaterialColumn(page);

    Require(
        debug.Has(
            terrain_debug::TerrainDebugField::BedrockType) &&
        debug.Has(
            terrain_debug::TerrainDebugField::Soil) &&
        debug.Has(
            terrain_debug::TerrainDebugField::ExposedMaterial),
        "M08 binding must expose physical material and exposed-surface fields.");

    const auto soil =
        debug.View(
            terrain_debug::TerrainDebugField::Soil);
    Require(
        soil.scalar.size() == 4U &&
        soil.scalar[0] == 0.5F &&
        soil.scalar[1] == 1.5F,
        "M08 soil debug data must preserve physical cell values.");
}

void TestExplicitTypedBindingAndProvenance()
{
    terrain_debug::TerrainDebugPageData debug(
        MakeStamp(),
        2,
        2);

    const std::vector<f32> uplift{
        -10.0F, 0.0F, 25.0F, 50.0F};
    debug.SetScalar(
        terrain_debug::TerrainDebugField::Uplift,
        uplift);

    Require(
        debug.View(
            terrain_debug::TerrainDebugField::Uplift).
                scalar[2] == 25.0F,
        "Explicit geology binding must preserve source data.");

    const auto resident =
        debug.View(
            terrain_debug::TerrainDebugField::CacheResidency);
    const auto invalidation =
        debug.View(
            terrain_debug::TerrainDebugField::CacheInvalidation);
    const auto lod =
        debug.View(
            terrain_debug::TerrainDebugField::PhysicalLod);

    Require(
        resident.boolean.size() == 4U &&
        resident.boolean[0] == 1U,
        "Cache residency must derive from page provenance.");

    Require(
        invalidation.revision[0] == 11U &&
        lod.lod[0] == 2U,
        "Invalidation and physical LOD must derive from the physical page stamp.");

    bool rejected = false;
    try
    {
        debug.SetScalar(
            terrain_debug::TerrainDebugField::Drainage,
            uplift);
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }

    Require(
        rejected,
        "Value-class mismatches must be rejected instead of reinterpreting terrain data.");
}

void TestMissingFieldIsExplicit()
{
    terrain_debug::TerrainDebugPageData debug(
        MakeStamp(),
        1,
        1);

    Require(
        !debug.Has(
            terrain_debug::TerrainDebugField::BiomeWeights),
        "Unavailable live products must not be synthesized.");

    bool threw = false;
    try
    {
        static_cast<void>(
            debug.View(
                terrain_debug::TerrainDebugField::BiomeWeights));
    }
    catch (const std::logic_error&)
    {
        threw = true;
    }

    Require(
        threw,
        "Requesting an unavailable debug field must fail explicitly.");
}
} // namespace

int main()
{
    TestMaterialColumnBinding();
    TestExplicitTypedBindingAndProvenance();
    TestMissingFieldIsExplicit();

    std::cout << "Orbit M29 live page binding tests passed.\n";
    return EXIT_SUCCESS;
}
