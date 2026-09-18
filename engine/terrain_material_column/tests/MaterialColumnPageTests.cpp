#include <orbit/rhi/Resource.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_impacts/ImpactField.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
using namespace orbit;
using namespace orbit::terrain_material_column;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M08 failure: " << message << '\n';
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
        Fail(
            message + " (" +
            std::to_string(a) + " vs " +
            std::to_string(b) + ")");
    }
}

terrain_geology::GeologicalMaterialLibrary MakeGeology()
{
    terrain_geology::GeologicalMaterialLibrary library;

    terrain_geology::GeologicalMaterial basalt{
        .id = terrain_geology::reference_rock::Basalt,
        .name = "M08 Basalt",
        .hardness = 0.9F,
        .cohesion = 0.9F,
        .hydraulicErodibility = 0.15F,
        .aeolianErodibility = 0.05F,
        .permeability = 0.12F,
        .chemicalWeatherability = 0.2F,
        .fractureTendency = 0.4F,
        .density = 3'000.0F
    };

    library.Upsert(basalt);
    return library;
}

MaterialColumnCell BaseCell()
{
    return {
        .bedrockHeightMeters = 100.0F,
        .referenceBedrockHeightMeters = 100.0F,
        .bedrockMaterial =
            terrain_geology::reference_rock::Basalt,
        .regolithMeters = 1.0F,
        .soilMeters = 1.0F,
        .sandMeters = 1.0F,
        .debrisMeters = 1.0F,
        .moisture = 0.4F,
        .temporaryScalar = 0.0F
    };
}

void InitializePage(MaterialColumnPage& page)
{
    for (u32 y = 0; y < page.Resolution(); ++y)
    {
        for (u32 x = 0; x < page.Resolution(); ++x)
        {
            page.SetCell(x, y, BaseCell());
        }
    }
}

void TestTopDownErosionOrder()
{
    auto geology = MakeGeology();
    MaterialColumnPage page(1, 2.0);
    page.SetCell(0, 0, BaseCell());

    const auto first =
        page.Erode(0, 0, 3.5, geology);

    RequireNear(first.debrisMeters, 1.0, 1.0e-6,
        "Erosion must remove debris first.");
    RequireNear(first.sandMeters, 1.0, 1.0e-6,
        "Erosion must remove sand before soil/regolith/bedrock.");
    RequireNear(first.soilMeters, 1.0, 1.0e-6,
        "Erosion must remove soil before regolith/bedrock.");
    RequireNear(first.regolithMeters, 0.5, 1.0e-6,
        "Erosion must consume regolith only after upper loose layers.");
    RequireNear(first.bedrockMeters, 0.0, 1.0e-9,
        "Bedrock must not erode while loose material remains.");

    const auto& middle = page.At(0, 0);
    RequireNear(middle.bedrockHeightMeters, 100.0, 1.0e-6,
        "Bedrock height changed before upper layers were exhausted.");

    const auto second =
        page.Erode(0, 0, 1.0, geology);

    RequireNear(second.regolithMeters, 0.5, 1.0e-6,
        "Remaining regolith must erode before bedrock.");
    RequireNear(second.bedrockMeters, 0.5, 1.0e-6,
        "Only residual erosion demand may cut bedrock.");
    RequireNear(page.At(0, 0).bedrockHeightMeters, 99.5, 1.0e-5,
        "Bedrock excavation did not lower the physical substrate.");
}

void TestTectonicBedrockDisplacement()
{
    auto geology = MakeGeology();

    MaterialColumnPage page(1, 2.0);
    page.SetCell(0, 0, BaseCell());

    const f64 looseBefore =
        page.At(0, 0).LooseDepthMeters();

    page.DisplaceBedrock(
        0,
        0,
        12.5);

    RequireNear(
        page.At(0, 0).bedrockHeightMeters,
        112.5,
        1.0e-5,
        "Tectonic uplift must move the bedrock surface.");

    RequireNear(
        page.At(0, 0).referenceBedrockHeightMeters,
        112.5,
        1.0e-5,
        "Tectonic displacement must move the mass-accounting reference.");

    RequireNear(
        page.At(0, 0).LooseDepthMeters(),
        looseBefore,
        0.0,
        "Tectonic displacement must preserve overlying loose material.");

    RequireNear(
        page.QueryMass(geology).excavatedBedrockKg,
        0.0,
        1.0e-9,
        "Uplift must not be counted as negative/positive excavation.");

    page.DisplaceBedrock(
        0,
        0,
        -20.0);

    RequireNear(
        page.At(0, 0).bedrockHeightMeters,
        92.5,
        1.0e-5,
        "Tectonic subsidence must move bedrock downward without erosion.");

    RequireNear(
        page.QueryMass(geology).excavatedBedrockKg,
        0.0,
        1.0e-9,
        "Subsidence with reference rebasing must not count as excavation.");
}

void TestMassAccounting()
{
    auto geology = MakeGeology();

    LooseMaterialDensities densities{
        .regolithKgPerCubicMeter = 1'500.0F,
        .soilKgPerCubicMeter = 1'200.0F,
        .sandKgPerCubicMeter = 1'600.0F,
        .debrisKgPerCubicMeter = 1'800.0F
    };

    MaterialColumnPage page(1, 2.0, densities);
    page.SetCell(0, 0, BaseCell());

    const auto initial =
        page.QueryMass(geology);

    const f64 area = 4.0;
    const f64 expectedLoose =
        area * (1'500.0 + 1'200.0 + 1'600.0 + 1'800.0);

    RequireNear(
        initial.LooseMassKg(),
        expectedLoose,
        1.0e-3,
        "Queryable loose-material mass is incorrect.");

    const auto removal =
        page.Erode(0, 0, 4.5, geology);

    const auto after =
        page.QueryMass(geology);

    const f64 expectedBedrockExcavated =
        0.5 * area * 3'000.0;

    RequireNear(
        after.excavatedBedrockKg,
        expectedBedrockExcavated,
        0.1,
        "Bedrock excavation mass must use M02 rock density.");

    RequireNear(
        removal.removedMassKg,
        expectedLoose + expectedBedrockExcavated,
        0.1,
        "Removal ledger must account for loose and bedrock mass.");

    RequireNear(
        after.LooseMassKg(),
        0.0,
        1.0e-6,
        "All loose material should be absent after 4.5 m erosion.");
}

void TestDepositionCoversExposedRock()
{
    auto geology = MakeGeology();
    MaterialColumnPage page(1, 1.0);

    auto cell = BaseCell();
    cell.regolithMeters = 0.0F;
    cell.soilMeters = 0.0F;
    cell.sandMeters = 0.0F;
    cell.debrisMeters = 0.0F;

    page.SetCell(0, 0, cell);

    Require(
        page.At(0, 0).ExposedSurface() ==
            ExposedSurfaceKind::Bedrock,
        "Zero loose depth must expose bedrock.");

    const f64 bedrockBefore =
        page.At(0, 0).bedrockHeightMeters;

    const f64 depositedMass =
        page.Deposit(
            0,
            0,
            LooseMaterialKind::Sand,
            0.4);

    Require(
        depositedMass > 0.0,
        "Deposition must report positive added mass.");

    Require(
        page.At(0, 0).ExposedSurface() ==
            ExposedSurfaceKind::Sand,
        "Deposited sediment must cover previously exposed rock.");

    RequireNear(
        page.At(0, 0).bedrockHeightMeters,
        bedrockBefore,
        0.0,
        "Loose deposition must not rewrite substrate height.");
}

void TestImpactChannelsFeedMaterialColumn()
{
    auto geology = MakeGeology();
    MaterialColumnPage page(1, 1.0);

    auto cell = BaseCell();
    cell.regolithMeters = 0.5F;
    cell.soilMeters = 0.0F;
    cell.sandMeters = 0.0F;
    cell.debrisMeters = 0.0F;
    page.SetCell(0, 0, cell);

    const terrain_impacts::CraterProcessSample impact{
        .heightDeltaMeters = -1.0,
        .excavationDepthMeters = 1.5,
        .ejectaThicknessMeters = 0.25,
        .debrisField = 0.6,
        .rayField = 0.7,
        .affectingImpacts = 1
    };

    const auto removal =
        page.ApplyImpact(
            0,
            0,
            impact,
            geology);

    RequireNear(
        removal.regolithMeters,
        0.5,
        1.0e-6,
        "M07 excavation must remove loose regolith first.");
    RequireNear(
        removal.bedrockMeters,
        1.0,
        1.0e-6,
        "M07 residual excavation must cut bedrock.");

    RequireNear(
        page.At(0, 0).debrisMeters,
        0.25,
        1.0e-5,
        "M07 ejecta must become M08 debris depth.");

    RequireNear(
        page.At(0, 0).temporaryScalar,
        0.7,
        1.0e-5,
        "M07 ray field must reach the temporary process channel.");
}

void TestGpuPackingAndFormats()
{
    auto geology = MakeGeology();
    MaterialColumnPage page(2, 4.0);
    InitializePage(page);

    page.At(0, 0).regolithMeters = 0.125F;
    page.At(0, 0).soilMeters = 0.25F;
    page.At(0, 0).sandMeters = 0.5F;
    page.At(0, 0).debrisMeters = 1.0F;
    page.At(0, 0).moisture = 0.75F;
    page.At(0, 0).temporaryScalar = 0.33F;

    const auto table = geology.BuildGpuTable();
    const auto packed =
        PackGpuPage(page, table);

    Require(
        packed.TexelCount() == 4,
        "Packed GPU page texel count is incorrect.");
    Require(
        packed.PackedByteSize() == 4 * 18,
        "M08 GPU page must pack to 18 bytes/texel across the four lanes.");

    const auto& loose = packed.looseRgba16F[0];

    RequireNear(
        HalfBitsToFloat(loose.regolith),
        0.125,
        1.0e-4,
        "R16F regolith packing is incorrect.");
    RequireNear(
        HalfBitsToFloat(loose.soil),
        0.25,
        1.0e-4,
        "R16F soil packing is incorrect.");
    RequireNear(
        HalfBitsToFloat(loose.sand),
        0.5,
        1.0e-4,
        "R16F sand packing is incorrect.");
    RequireNear(
        HalfBitsToFloat(loose.debris),
        1.0,
        1.0e-4,
        "R16F debris packing is incorrect.");

    Require(
        packed.geologicalMaterialR16Uint[0] == 0,
        "Single-rock M02 GPU table should encode as R16_UINT index zero.");

    Require(
        rhi::TextureFormatBytesPerTexel(
            rhi::TextureFormat::R32_Float) == 4 &&
        rhi::TextureFormatBytesPerTexel(
            rhi::TextureFormat::RGBA16_Float) == 8 &&
        rhi::TextureFormatBytesPerTexel(
            rhi::TextureFormat::RG16_Float) == 4 &&
        rhi::TextureFormatBytesPerTexel(
            rhi::TextureFormat::R16_UInt) == 2,
        "RHI texel sizes do not match the M08 GPU layout.");
}

void TestFixedSurfaceStack()
{
    MaterialColumnCell cell = BaseCell();

    Require(
        cell.ExposedSurface() == ExposedSurfaceKind::Debris,
        "Debris must be topmost in the fixed M08 stack.");

    cell.debrisMeters = 0.0F;
    Require(
        cell.ExposedSurface() == ExposedSurfaceKind::Sand,
        "Sand must be exposed after debris is removed.");

    cell.sandMeters = 0.0F;
    Require(
        cell.ExposedSurface() == ExposedSurfaceKind::Soil,
        "Soil must be exposed after transported sediment is removed.");

    cell.soilMeters = 0.0F;
    Require(
        cell.ExposedSurface() == ExposedSurfaceKind::Regolith,
        "Regolith must be exposed below soil.");

    cell.regolithMeters = 0.0F;
    Require(
        cell.ExposedSurface() == ExposedSurfaceKind::Bedrock,
        "Bedrock must be exposed only after all loose layers are gone.");
}
} // namespace

int main()
{
    TestTopDownErosionOrder();
    TestTectonicBedrockDisplacement();
    TestMassAccounting();
    TestDepositionCoversExposedRock();
    TestImpactChannelsFeedMaterialColumn();
    TestGpuPackingAndFormats();
    TestFixedSurfaceStack();

    std::cout << "Orbit M08 material-column tests passed.\n";
    return EXIT_SUCCESS;
}
