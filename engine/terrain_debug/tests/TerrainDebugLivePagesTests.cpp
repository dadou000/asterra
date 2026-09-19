#include <orbit/terrain_debug/TerrainDebugLivePages.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M29 live-page registry failure: "
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

terrain::PhysicalTerrainPageAddress Address()
{
    return {
        .planet = {
            .high = 0x4d32394c49564550ULL,
            .low = 0x4147455445535401ULL
        },
        .tile = {
            .face = world::CubeFace::PositiveZ,
            .level = 8,
            .x = 77,
            .y = 91
        }
    };
}

std::shared_ptr<const terrain_debug::TerrainDebugPageData>
Build(const u64 geologyRevision)
{
    terrain_material_column::MaterialColumnPage material(
        2,
        5.0);

    for (u32 y = 0; y < 2; ++y)
    {
        for (u32 x = 0; x < 2; ++x)
        {
            terrain_material_column::MaterialColumnCell cell{};
            cell.bedrockHeightMeters = 20.0F;
            cell.referenceBedrockHeightMeters = 20.0F;
            cell.bedrockMaterial = {
                .high = 10U + x,
                .low = 20U + y
            };
            cell.soilMeters =
                0.25F +
                static_cast<f32>(x);
            cell.moisture = 0.4F;
            material.SetCell(x, y, cell);
        }
    }

    std::vector<terrain_macro_geology::MacroGeologySample>
        geology(4);
    geology[0].upliftMeters = 125.0;

    const terrain_debug::TerrainDebugLivePageInputs inputs{
        .address = Address(),
        .physicalLod = 2,
        .revisions = {
            .geology = geologyRevision,
            .climate = 2,
            .authoring = 3,
            .biome = 4,
            .water = 5,
            .processes = 6
        },
        .cacheResident = true,
        .invalidationRevision = 9,
        .width = 2,
        .height = 2,
        .materialColumn = &material,
        .macroGeology = geology
    };

    return terrain_debug::CaptureLiveTerrainDebugPage(
        inputs);
}

void TestCaptureUsesActualProducts()
{
    const auto page = Build(1);

    Require(
        page->Stamp().address == Address() &&
        page->Stamp().physicalLod == 2,
        "Capture must preserve physical page identity and physical LOD.");

    Require(
        page->View(
            terrain_debug::TerrainDebugField::Soil).
                scalar[1] == 1.25F,
        "M08 live capture must copy the actual material-column value.");

    Require(
        page->View(
            terrain_debug::TerrainDebugField::Uplift).
                scalar[0] == 125.0F,
        "M05 live capture must copy the actual macro-geology value.");

    Require(
        !page->Has(
            terrain_debug::TerrainDebugField::Drainage),
        "An absent live producer must remain explicitly unavailable.");
}

void TestAddressLookupReplacesCurrentSnapshot()
{
    terrain_debug::TerrainDebugLivePages pages;

    const auto first = Build(1);
    pages.Publish(first);

    Require(
        pages.Size() == 1U &&
        pages.Find(Address()) == first,
        "Published live physical page must resolve by stable address.");

    const auto replacement = Build(7);
    pages.Publish(replacement);

    const auto found = pages.Find(Address());
    Require(
        pages.Size() == 1U &&
        found == replacement &&
        found->Stamp().revisions.geology == 7U,
        "Republishing the same address must expose the current immutable snapshot.");

    auto missing = Address();
    ++missing.tile.x;

    Require(
        pages.Find(missing) == nullptr,
        "Unpublished physical pages must not be synthesized.");

    Require(
        pages.Erase(Address()) &&
        pages.Size() == 0U,
        "Erasing a physical address must remove its live debug snapshot.");
}
} // namespace

int main()
{
    TestCaptureUsesActualProducts();
    TestAddressLookupReplacesCurrentSnapshot();

    std::cout << "Orbit M29 live-page registry tests passed.\n";
    return EXIT_SUCCESS;
}
