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

    terrain_erosion::SedimentExchangePage sediment(
        2,
        5.0);

    sediment.RecordTransport(
        0,
        0,
        1,
        0,
        terrain_erosion::SedimentTransportMedium::Waterborne,
        terrain_erosion::SedimentMass{
            .sandKg = 100.0});

    sediment.RecordTransport(
        0,
        0,
        0,
        -1,
        terrain_erosion::SedimentTransportMedium::Airborne,
        terrain_erosion::SedimentMass{
            .finesKg = 50.0});

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
        .sedimentExchange = &sediment,
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

    const auto sedimentFlux =
        page->View(
            terrain_debug::TerrainDebugField::SedimentFlux);

    Require(
        sedimentFlux.vector[0].x == 4.0F &&
        sedimentFlux.vector[0].y == 2.0F,
        "M14 live capture must expose actual transported mass density in east/north orientation.");

    Require(
        !page->Has(
            terrain_debug::TerrainDebugField::Drainage),
        "An absent live producer must remain explicitly unavailable.");
}

void TestAddressLookupReplacesCurrentSnapshot()
{
    terrain_debug::TerrainDebugLivePages pages;

    const auto first = Build(1);
    Require(
        pages.Publish(first),
        "First live page publication must be accepted.");

    Require(
        pages.Size() == 1U &&
        pages.Find(Address()) == first,
        "Published live physical page must resolve by stable address.");

    const auto replacement = Build(7);
    Require(
        pages.Publish(replacement),
        "Newer live page publication must replace the current snapshot.");

    const auto stale = Build(2);
    Require(
        !pages.Publish(stale),
        "Older authority revisions must be rejected instead of replacing a current live page.");

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
