#include <orbit/surface_authoring/TerrainConstraints.hpp>
#include <orbit/surface_model/SurfaceMaterialResolver.hpp>
#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_geology/Stratigraphy.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>
#include <orbit/terrain_erosion/AeolianErosion.hpp>
#include <orbit/terrain_erosion/HydraulicErosion.hpp>
#include <orbit/terrain_erosion/SedimentExchange.hpp>
#include <orbit/terrain_erosion/ThermalErosion.hpp>
#include <orbit/terrain_macro_geology/MacroGeologyField.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/terrain_material_column/SurfaceResolver.hpp>
#include <orbit/terrain_region/SurfaceBoundaryExchange.hpp>
#include <orbit/terrain_render/SurfaceMaterial.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifndef ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR
#error ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR must be defined for M30 validation.
#endif

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "V0.0.4 M30 validation failure: "
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

bool NearlyEqual(
    const f64 a,
    const f64 b,
    const f64 epsilon = 1.0e-5)
{
    return std::abs(a - b) <= epsilon;
}

struct StratigraphyFixture
{
    terrain_geology::GeologicalMaterialLibrary materials;
    terrain_geology::CompiledStratigraphyProfile stratigraphy;
    world::PlanetDefinition planet;
    terrain::PlanetSurfacePosition anchor;
    f64 topMeters{0.0};

    StratigraphyFixture()
        : materials(
              terrain_geology::LoadGeologicalMaterialDirectory(
                  std::filesystem::path{
                      ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR})),
          stratigraphy(
              terrain_geology::LoadStratigraphyProfileFile(
                  std::filesystem::path{
                      ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR} /
                  "continental_shelf.orbitstratigraphy"),
              materials),
          planet{
              .radiusMeters = 6'000'000.0,
              .id = {
                  .high = 0x4D333056414C3031ULL,
                  .low = 0x5354524154410001ULL
              },
              .generationSeed = 0x303001ULL},
          anchor{
              .planet = planet.id,
              .unitDirection =
                  math::Normalize(
                      stratigraphy.Profile().
                          transform.anchorUnitDirection),
              .radialOffsetMeters = 0.0}
    {
        topMeters =
            stratigraphy.DeformedTopRadialOffsetMeters(
                planet,
                anchor);
    }

    [[nodiscard]] terrain_geology::StratigraphySample
    SampleBedrock(
        const terrain_material_column::MaterialColumnPage& page) const
    {
        auto position = anchor;
        position.radialOffsetMeters =
            static_cast<f64>(
                page.At(0U, 0U).
                    bedrockHeightMeters);

        return stratigraphy.SampleExposedLayer(
            planet,
            position);
    }

    [[nodiscard]] terrain_material_column::MaterialColumnPage
    MakePage() const
    {
        terrain_material_column::MaterialColumnPage page(
            1U,
            2.0);

        auto position = anchor;
        position.radialOffsetMeters =
            topMeters - 0.5;

        const auto sample =
            stratigraphy.SampleExposedLayer(
                planet,
                position);

        page.SetCell(
            0U,
            0U,
            {
                .bedrockHeightMeters =
                    static_cast<f32>(
                        position.radialOffsetMeters),
                .referenceBedrockHeightMeters =
                    static_cast<f32>(
                        position.radialOffsetMeters),
                .bedrockMaterial =
                    sample.primaryMaterial,
                .regolithMeters = 0.0F,
                .soilMeters = 0.0F,
                .sandMeters = 0.0F,
                .debrisMeters = 0.0F,
                .moisture = 0.0F
            });

        return page;
    }
};

void Test01StratigraphyExposure()
{
    using namespace terrain_geology;

    StratigraphyFixture fixture;

    auto first =
        fixture.MakePage();
    auto replay =
        fixture.MakePage();

    const auto initial =
        fixture.SampleBedrock(first);

    Require(
        initial.primaryMaterial ==
            reference_rock::VolcanicAsh &&
        initial.layerIndex == 0U &&
        !initial.basement,
        "M30-01 setup must begin inside the weak volcanic-ash layer.");

    const auto firstRemoval =
        first.Erode(
            0U,
            0U,
            2.0,
            fixture.materials);

    const auto replayRemoval =
        replay.Erode(
            0U,
            0U,
            2.0,
            fixture.materials);

    const auto middle =
        fixture.SampleBedrock(first);
    const auto middleReplay =
        fixture.SampleBedrock(replay);

    Require(
        NearlyEqual(
            firstRemoval.bedrockMeters,
            2.0) &&
        middle.primaryMaterial ==
            reference_rock::Sandstone &&
        middle.layerIndex == 1U &&
        !middle.basement,
        "M30-01 lowering the physical M08 bedrock surface through the first virtual boundary must expose sandstone.");

    Require(
        first.At(0U, 0U).
                bedrockHeightMeters ==
            replay.At(0U, 0U).
                bedrockHeightMeters &&
        firstRemoval.removedMassKg ==
            replayRemoval.removedMassKg &&
        middle.primaryMaterial ==
            middleReplay.primaryMaterial &&
        middle.layerIndex ==
            middleReplay.layerIndex,
        "M30-01 stratigraphy exposure must be deterministic for identical physical erosion.");

    const auto deepRemoval =
        first.Erode(
            0U,
            0U,
            6.0,
            fixture.materials);

    const auto deep =
        fixture.SampleBedrock(first);

    Require(
        NearlyEqual(
            deepRemoval.bedrockMeters,
            6.0) &&
        deep.primaryMaterial ==
            reference_rock::Limestone &&
        deep.layerIndex == 2U &&
        !deep.basement,
        "M30-01 continued physical incision must advance into the limestone layer rather than remaining phase-locked to the original stratum.");

    Require(
        NearlyEqual(
            deep.depthBelowTopMeters,
            8.5,
            1.0e-4),
        "M30-01 virtual stratigraphic depth must track the eroded physical bedrock height.");
}

void Test02BedrockStripping()
{
    using namespace terrain_material_column;

    StratigraphyFixture fixture;
    auto page =
        fixture.MakePage();

    auto& cell =
        page.At(0U, 0U);

    // Binary-exact layer depths keep the exact-cover threshold deterministic:
    // debris -> sand -> soil -> regolith -> bedrock.
    cell.regolithMeters = 0.5F;
    cell.soilMeters = 0.25F;
    cell.sandMeters = 0.125F;
    cell.debrisMeters = 0.125F;

    const f64 bedrockBefore =
        cell.bedrockHeightMeters;
    const f64 referenceBefore =
        cell.referenceBedrockHeightMeters;

    const auto& densities =
        page.Densities();
    const f64 area =
        page.CellAreaSquareMeters();

    const f64 expectedLooseMass =
        area *
        (0.5 * densities.regolithKgPerCubicMeter +
         0.25 * densities.soilKgPerCubicMeter +
         0.125 * densities.sandKgPerCubicMeter +
         0.125 * densities.debrisKgPerCubicMeter);

    Require(
        cell.ExposedSurface() ==
            ExposedSurfaceKind::Debris,
        "M30-02 setup must begin with the complete loose-material stack covering bedrock.");

    const auto partial =
        page.Erode(
            0U,
            0U,
            0.75,
            fixture.materials);

    Require(
        NearlyEqual(partial.debrisMeters, 0.125) &&
        NearlyEqual(partial.sandMeters, 0.125) &&
        NearlyEqual(partial.soilMeters, 0.25) &&
        NearlyEqual(partial.regolithMeters, 0.25) &&
        NearlyEqual(partial.bedrockMeters, 0.0),
        "M30-02 stripping must consume debris, sand and soil before regolith, without touching bedrock while loose cover remains.");

    Require(
        page.At(0U, 0U).ExposedSurface() ==
            ExposedSurfaceKind::Regolith &&
        NearlyEqual(
            page.At(0U, 0U).LooseDepthMeters(),
            0.25) &&
        NearlyEqual(
            page.At(0U, 0U).bedrockHeightMeters,
            bedrockBefore),
        "M30-02 partial stripping must leave the surviving regolith exposed and preserve the physical bedrock surface.");

    const auto finish =
        page.Erode(
            0U,
            0U,
            0.25,
            fixture.materials);

    Require(
        NearlyEqual(finish.regolithMeters, 0.25) &&
        NearlyEqual(finish.bedrockMeters, 0.0) &&
        page.At(0U, 0U).ExposedSurface() ==
            ExposedSurfaceKind::Bedrock,
        "M30-02 exact removal of the final loose cover must expose bedrock without consuming bedrock.");

    Require(
        NearlyEqual(
            page.At(0U, 0U).bedrockHeightMeters,
            bedrockBefore) &&
        NearlyEqual(
            page.At(0U, 0U).referenceBedrockHeightMeters,
            referenceBefore),
        "M30-02 stripping loose cover must not move either the current or reference bedrock surface.");

    const auto strippedMass =
        page.QueryMass(
            fixture.materials);

    Require(
        NearlyEqual(
            partial.removedMassKg +
                finish.removedMassKg,
            expectedLooseMass) &&
        NearlyEqual(
            strippedMass.LooseMassKg(),
            0.0) &&
        NearlyEqual(
            strippedMass.excavatedBedrockKg,
            0.0),
        "M30-02 loose-cover stripping must conserve the removed loose mass without reporting false bedrock excavation.");

    const auto* rock =
        fixture.materials.Find(
            page.At(0U, 0U).bedrockMaterial);

    Require(
        rock != nullptr,
        "M30-02 exposed bedrock must retain a valid M02 geological material identity.");

    const auto bedrockCut =
        page.Erode(
            0U,
            0U,
            0.5,
            fixture.materials);

    const auto afterCutMass =
        page.QueryMass(
            fixture.materials);

    Require(
        NearlyEqual(
            bedrockCut.bedrockMeters,
            0.5) &&
        NearlyEqual(
            page.At(0U, 0U).bedrockHeightMeters,
            bedrockBefore - 0.5) &&
        page.At(0U, 0U).ExposedSurface() ==
            ExposedSurfaceKind::Bedrock,
        "M30-02 only erosion demand after complete stripping may lower the physical bedrock surface.");

    Require(
        NearlyEqual(
            afterCutMass.excavatedBedrockKg,
            0.5 * area * rock->density,
            1.0e-3),
        "M30-02 post-exposure bedrock excavation must enter M08 mass accounting using the exposed M02 rock density.");
}


void Test03SedimentDepositionBurial()
{
    using namespace terrain_erosion;
    using namespace terrain_material_column;

    StratigraphyFixture fixture;
    auto page =
        fixture.MakePage();

    const auto initialStratum =
        fixture.SampleBedrock(page);

    const auto& initialCell =
        page.At(0U, 0U);

    Require(
        initialCell.ExposedSurface() ==
            ExposedSurfaceKind::Bedrock &&
        NearlyEqual(
            initialCell.LooseDepthMeters(),
            0.0),
        "M30-03 setup must begin with genuinely exposed bedrock.");

    const f64 bedrockBefore =
        initialCell.bedrockHeightMeters;
    const f64 referenceBefore =
        initialCell.referenceBedrockHeightMeters;
    const auto materialBefore =
        initialCell.bedrockMaterial;
    const f64 surfaceBefore =
        initialCell.SurfaceHeightMeters();

    SedimentExchangePage sediment(
        page.Resolution(),
        page.SpacingMeters());

    const f64 area =
        page.CellAreaSquareMeters();
    const auto& densities =
        page.Densities();

    // Choose binary-exact physical depths so the M14 -> M08 conversion can be
    // checked without a tolerance-sensitive boundary:
    // 0.125 m debris + 0.250 m sand + 0.500 m fines/soil.
    const SedimentMass load{
        .sandKg =
            0.25 *
            area *
            densities.sandKgPerCubicMeter,
        .finesKg =
            0.5 *
            area *
            densities.soilKgPerCubicMeter,
        .coarseDebrisKg =
            0.125 *
            area *
            densities.debrisKgPerCubicMeter
    };

    sediment.Add(
        0U,
        0U,
        SedimentTransportMedium::Waterborne,
        load);

    Require(
        NearlyEqual(
            sediment.TotalMobileMass().TotalKg(),
            load.TotalKg()),
        "M30-03 typed sediment must exist in M14 mobile state before physical deposition.");

    const auto burial =
        sediment.DepositToColumn(
            page,
            0U,
            0U,
            SedimentTransportMedium::Waterborne);

    const auto& buried =
        page.At(0U, 0U);

    Require(
        NearlyEqual(
            burial.requested.sandKg,
            load.sandKg) &&
        NearlyEqual(
            burial.requested.finesKg,
            load.finesKg) &&
        NearlyEqual(
            burial.requested.coarseDebrisKg,
            load.coarseDebrisKg) &&
        NearlyEqual(
            burial.deposited.TotalKg(),
            load.TotalKg()) &&
        burial.remaining.Empty(1.0e-9) &&
        sediment.TotalMobileMass().Empty(1.0e-9),
        "M30-03 complete burial must transfer the full typed M14 load back into M08 without duplicated mobile mass.");

    Require(
        NearlyEqual(
            buried.debrisMeters,
            0.125) &&
        NearlyEqual(
            buried.sandMeters,
            0.25) &&
        NearlyEqual(
            buried.soilMeters,
            0.5) &&
        NearlyEqual(
            buried.regolithMeters,
            0.0) &&
        NearlyEqual(
            buried.LooseDepthMeters(),
            0.875),
        "M30-03 M14 sediment classes must resolve into the canonical M08 debris/sand/soil physical layers.");

    Require(
        buried.ExposedSurface() ==
            ExposedSurfaceKind::Debris &&
        NearlyEqual(
            buried.SurfaceHeightMeters(),
            surfaceBefore + 0.875),
        "M30-03 deposited sediment must physically bury bedrock and raise the M08 surface by the deposited loose depth.");

    Require(
        NearlyEqual(
            buried.bedrockHeightMeters,
            bedrockBefore) &&
        NearlyEqual(
            buried.referenceBedrockHeightMeters,
            referenceBefore) &&
        buried.bedrockMaterial ==
            materialBefore,
        "M30-03 burial must cover geological substrate without rewriting bedrock height, excavation reference or M02 rock identity.");

    const auto buriedStratum =
        fixture.SampleBedrock(page);

    Require(
        buriedStratum.primaryMaterial ==
            initialStratum.primaryMaterial &&
        buriedStratum.layerIndex ==
            initialStratum.layerIndex &&
        buriedStratum.basement ==
            initialStratum.basement,
        "M30-03 loose sediment burial must not rewrite the underlying M03 stratigraphic identity.");

    const auto accounting =
        sediment.Accounting();

    Require(
        accounting.physicalToMobile.Empty(1.0e-9) &&
        NearlyEqual(
            accounting.mobileToPhysical.sandKg,
            load.sandKg) &&
        NearlyEqual(
            accounting.mobileToPhysical.finesKg,
            load.finesKg) &&
        NearlyEqual(
            accounting.mobileToPhysical.coarseDebrisKg,
            load.coarseDebrisKg),
        "M30-03 M14 accounting must report the burial exactly once as mobile-to-physical transfer.");

    const auto physicalMass =
        page.QueryMass(
            fixture.materials);

    Require(
        NearlyEqual(
            physicalMass.LooseMassKg(),
            load.TotalKg(),
            1.0e-3) &&
        NearlyEqual(
            physicalMass.excavatedBedrockKg,
            0.0),
        "M30-03 buried sediment mass must reside in M08 while bedrock excavation remains zero.");

    const auto reExposure =
        page.Erode(
            0U,
            0U,
            0.875,
            fixture.materials);

    const auto& exposedAgain =
        page.At(0U, 0U);

    Require(
        NearlyEqual(
            reExposure.removedMassKg,
            load.TotalKg(),
            1.0e-3) &&
        NearlyEqual(
            reExposure.bedrockMeters,
            0.0) &&
        exposedAgain.ExposedSurface() ==
            ExposedSurfaceKind::Bedrock &&
        NearlyEqual(
            exposedAgain.bedrockHeightMeters,
            bedrockBefore) &&
        exposedAgain.bedrockMaterial ==
            materialBefore,
        "M30-03 stripping only the deposited sediment must re-expose the same bedrock without geological incision.");

    const auto finalStratum =
        fixture.SampleBedrock(page);

    Require(
        finalStratum.primaryMaterial ==
            initialStratum.primaryMaterial &&
        finalStratum.layerIndex ==
            initialStratum.layerIndex,
        "M30-03 burial and re-exposure must leave the virtual stratigraphic substrate unchanged.");
}


void Test04HydraulicMassConservation()
{
    using namespace terrain_erosion;
    using namespace terrain_material_column;

    constexpr u32 resolution = 7U;
    constexpr f64 spacingMeters = 10.0;

    StratigraphyFixture fixture;

    MaterialColumnPage page(
        resolution,
        spacingMeters);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            page.SetCell(
                x,
                y,
                {
                    .bedrockHeightMeters =
                        30.0F -
                        static_cast<f32>(x) *
                            0.8F,
                    .referenceBedrockHeightMeters =
                        0.0F,
                    .bedrockMaterial =
                        terrain_geology::
                            reference_rock::
                                VolcanicAsh,
                    .regolithMeters = 0.20F,
                    .soilMeters = 0.20F,
                    .sandMeters = 0.0F,
                    .debrisMeters = 0.0F,
                    .moisture = 0.0F,
                    .temporaryScalar = 0.0F
                });
        }
    }

    const auto initialMass =
        page.QueryMass(
            fixture.materials);

    std::vector<f32> rainfallSources(
        static_cast<std::size_t>(
            resolution) *
            resolution,
        0.0F);

    for (u32 y = 1U;
         y + 1U < resolution;
         ++y)
    {
        rainfallSources[
            static_cast<std::size_t>(y) *
                resolution] =
            0.006F;
    }

    HydraulicErosionConfig config{};
    config.iterations = 80U;
    config.timeStepSeconds = 0.15;
    config.rainfallMetersPerSecond = 0.0025;
    config.gravityMetersPerSecondSquared = 9.81;
    config.pipeCrossSectionSquareMeters = 0.20;
    config.sedimentCapacityCoefficient = 1'200.0;
    config.maximumSedimentConcentrationKgPerCubicMeter =
        1'500.0;
    config.erosionRatePerSecond = 1.0;
    config.depositionRatePerSecond = 1.4;
    config.maximumErosionDepthPerStepMeters = 0.04;
    config.maximumDepositionDepthPerStepMeters = 0.04;
    config.infiltrationMetersPerSecond = 0.00008;
    config.moistureCapacityDepthMeters = 0.12;
    config.evaporationRatePerSecond = 0.08;

    const auto result =
        SimulateHydraulicErosion(
            std::move(page),
            fixture.materials,
            rainfallSources,
            config);

    Require(
        result.sedimentExchange.has_value(),
        "M30-04 hydraulic erosion must publish its mobile sediment through the canonical M14 exchange page.");

    const auto& balance =
        result.massBalance;
    const auto& sediment =
        *result.sedimentExchange;

    Require(
        balance.totalErodedKg > 1.0 &&
        balance.totalDepositedKg > 0.0 &&
        balance.finalSuspendedKg > 0.0,
        "M30-04 scenario must exercise erosion, redeposition and remaining suspended load in one closed hydraulic solve.");

    Require(
        NearlyEqual(
            balance.sedimentBoundaryLossKg,
            0.0) &&
        sediment.Accounting().
            imported.Empty(1.0e-9) &&
        sediment.Accounting().
            exported.Empty(1.0e-9),
        "M30-04 closed physical page must not hide hydraulic sediment mass in boundary import/export accounting.");

    const f64 processTolerance =
        std::max(
            balance.totalErodedKg *
                1.0e-9,
            1.0e-6);

    Require(
        NearlyEqual(
            balance.totalErodedKg,
            balance.totalDepositedKg +
                balance.finalSuspendedKg,
            processTolerance) &&
        std::abs(
            balance.materialBalanceErrorKg) <=
            processTolerance &&
        balance.materialBalanceRelativeError <
            1.0e-9,
        "M30-04 hydraulic process ledger must close as eroded = deposited + final mobile sediment.");

    const auto& accounting =
        sediment.Accounting();

    Require(
        NearlyEqual(
            accounting.physicalToMobile.TotalKg(),
            balance.totalErodedKg,
            processTolerance) &&
        NearlyEqual(
            accounting.mobileToPhysical.TotalKg(),
            balance.totalDepositedKg,
            processTolerance) &&
        NearlyEqual(
            sediment.TotalMobileMass().TotalKg(),
            balance.finalSuspendedKg,
            processTolerance),
        "M30-04 M14 physical/mobile accounting must match the M11 hydraulic process ledger exactly.");

    f64 typedSuspendedKg = 0.0;
    f64 scalarSuspendedKg = 0.0;

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& hydraulic =
                result.At(x, y);
            const auto& shared =
                sediment.At(x, y).
                    waterborne;

            typedSuspendedKg +=
                shared.TotalKg();
            scalarSuspendedKg +=
                hydraulic.
                    suspendedSedimentKg;

            Require(
                NearlyEqual(
                    hydraulic.
                        suspendedSediment.
                        TotalKg(),
                    shared.TotalKg(),
                    1.0e-7) &&
                NearlyEqual(
                    hydraulic.
                        suspendedSedimentKg,
                    shared.TotalKg(),
                    1.0e-7),
                "M30-04 M11 typed/scalar diagnostics must remain synchronized to M14 waterborne authority per cell.");
        }
    }

    Require(
        NearlyEqual(
            typedSuspendedKg,
            balance.finalSuspendedKg,
            processTolerance) &&
        NearlyEqual(
            scalarSuspendedKg,
            balance.finalSuspendedKg,
            processTolerance),
        "M30-04 final suspended mass must have one value across the M11 ledger, M14 typed authority and scalar compatibility channel.");

    const auto finalMass =
        result.material.QueryMass(
            fixture.materials);

    const f64 physicalError =
        initialMass.LooseMassKg() +
        finalMass.excavatedBedrockKg -
        finalMass.LooseMassKg() -
        balance.finalSuspendedKg;

    const f64 physicalReference =
        std::max(
            initialMass.LooseMassKg() +
                finalMass.excavatedBedrockKg,
            1.0);

    Require(
        NearlyEqual(
            physicalError,
            balance.physicalColumnBalanceErrorKg,
            std::max(
                physicalReference *
                    1.0e-9,
                1.0e-4)) &&
        balance.physicalColumnBalanceRelativeError <
            2.0e-5,
        "M30-04 independent M08 physical-column accounting must close against final M14 suspended mass within the documented f32 layer tolerance.");
}


void Test05AeolianMassConservation()
{
    using namespace terrain_erosion;
    using namespace terrain_material_column;

    constexpr u32 resolution = 9U;
    constexpr f64 spacingMeters = 5.0;
    constexpr f32 initialSandMeters = 0.25F;

    StratigraphyFixture fixture;

    MaterialColumnPage page(
        resolution,
        spacingMeters);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const f32 obstacleHeight =
                x == 4U &&
                y >= 2U &&
                y <= 6U
                    ? 2.5F
                    : 0.0F;

            page.SetCell(
                x,
                y,
                {
                    .bedrockHeightMeters =
                        obstacleHeight,
                    .referenceBedrockHeightMeters =
                        0.0F,
                    .bedrockMaterial =
                        terrain_geology::
                            reference_rock::
                                Basalt,
                    .regolithMeters = 0.0F,
                    .soilMeters = 0.0F,
                    .sandMeters =
                        initialSandMeters,
                    .debrisMeters = 0.0F,
                    .moisture = 0.0F,
                    .temporaryScalar = 0.0F
                });
        }
    }

    const auto initialPhysical =
        page.QueryMass(
            fixture.materials);

    std::vector<AeolianCellForcing> wind(
        static_cast<std::size_t>(
            resolution) *
            resolution,
        AeolianCellForcing{
            .windEastMetersPerSecond = 14.0F,
            .windNorthMetersPerSecond = 0.0F,
            .surfaceResistance = 0.0F
        });

    AeolianErosionConfig config{};
    config.iterations = 120U;
    config.timeStepSeconds = 0.20;
    config.capacityCoefficient = 0.045;
    config.windSpeedExponent = 2.0;
    config.shadowRayCells = 4U;
    config.shadowStrength = 12.0;
    config.windwardExposureGain = 0.75;
    config.minimumExposure = 0.02;
    config.maximumExposure = 2.0;
    config.pickupRatePerSecond = 2.0;
    config.depositionRatePerSecond = 2.5;
    config.reptationFraction = 0.25;
    config.saltationRatePerSecond = 3.0;
    config.referenceSaltationWindMetersPerSecond = 12.0;
    config.maximumSandPickupDepthPerStepMeters = 0.03;
    config.maximumSoilPickupDepthPerStepMeters = 0.015;
    config.maximumDepositionDepthPerStepMeters = 0.05;
    config.moistureSuppressionExponent = 2.5;
    config.bedrockAbrasionMetersPerSecondAtReferenceWind =
        1.0e-6;
    config.maximumBedrockAbrasionDepthPerStepMeters =
        5.0e-5;

    const auto result =
        SimulateAeolianErosion(
            std::move(page),
            fixture.materials,
            wind,
            config);

    Require(
        result.sedimentExchange.has_value(),
        "M30-05 aeolian erosion must publish all mobile sediment through the canonical M14 exchange page.");

    const auto& sediment =
        *result.sedimentExchange;
    const auto& accounting =
        sediment.Accounting();
    const auto& balance =
        result.massBalance;

    f64 pickedSandKg = 0.0;
    f64 depositedKg = 0.0;
    f64 diagnosticAirborneKg = 0.0;
    f64 eastwardTransportKg = 0.0;
    f64 surfaceTransportKg = 0.0;

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& state =
                result.At(x, y);
            const auto& mobile =
                sediment.At(x, y);

            pickedSandKg +=
                state.cumulativeSandPickedKg;
            depositedKg +=
                state.cumulativeDepositedKg;
            diagnosticAirborneKg +=
                state.airborneSandKg +
                state.airborneFinesKg;

            const auto& transport =
                sediment.TransportAt(x, y);

            eastwardTransportKg +=
                std::max(
                    transport.airborne.eastKg,
                    0.0);
            surfaceTransportKg +=
                std::hypot(
                    transport.surfaceMobile.eastKg,
                    transport.surfaceMobile.northKg);

            Require(
                mobile.waterborne.Empty(1.0e-9),
                "M30-05 aeolian-only solve must not create waterborne M14 sediment.");
        }
    }

    Require(
        pickedSandKg > 0.0 &&
        depositedKg > 0.0 &&
        eastwardTransportKg > 0.0 &&
        accounting.exported.TotalKg() > 0.0,
        "M30-05 scenario must exercise sand pickup, physical deposition, directional wind transport and explicit page-boundary export.");

    Require(
        surfaceTransportKg > 0.0,
        "M30-05 reptation must pass through the M14 surface-mobile transport lane rather than an aeolian-private store.");

    const f64 referenceMass =
        std::max(
            balance.initialLooseMassKg +
                balance.abradedBedrockMassKg,
            1.0);

    const f64 physicalTolerance =
        std::max(
            referenceMass *
                3.0e-5,
            1.0e-4);

    const f64 recomputedPhysicalError =
        initialPhysical.LooseMassKg() +
        balance.abradedBedrockMassKg -
        balance.finalLooseMassKg -
        balance.finalAirborneMassKg -
        balance.boundaryLossKg;

    Require(
        NearlyEqual(
            balance.initialLooseMassKg,
            initialPhysical.LooseMassKg(),
            1.0e-6) &&
        NearlyEqual(
            recomputedPhysicalError,
            balance.materialBalanceErrorKg,
            physicalTolerance) &&
        balance.materialBalanceRelativeError <
            3.0e-5,
        "M30-05 M13 physical mass ledger must close as initial loose + abraded bedrock = final loose + mobile + exported mass.");

    const f64 mobileKg =
        sediment.TotalMobileMass().
            TotalKg();

    Require(
        NearlyEqual(
            mobileKg,
            balance.finalAirborneMassKg,
            physicalTolerance) &&
        NearlyEqual(
            accounting.exported.TotalKg(),
            balance.boundaryLossKg,
            physicalTolerance),
        "M30-05 M13 mass-balance outputs must be derived from the same M14 mobile inventory and boundary export ledger.");

    const f64 exchangeReference =
        std::max(
            accounting.physicalToMobile.
                TotalKg(),
            1.0);

    const f64 exchangeTolerance =
        std::max(
            exchangeReference *
                1.0e-9,
            1.0e-6);

    Require(
        accounting.imported.Empty(1.0e-9) &&
        NearlyEqual(
            accounting.physicalToMobile.
                TotalKg(),
            accounting.mobileToPhysical.
                    TotalKg() +
                accounting.exported.
                    TotalKg() +
                mobileKg,
            exchangeTolerance),
        "M30-05 M14 aeolian exchange must conserve physical pickup as redeposition + boundary export + remaining mobile sediment.");

    Require(
        NearlyEqual(
            diagnosticAirborneKg,
            mobileKg,
            physicalTolerance),
        "M30-05 M13 airborne diagnostic cells must mirror the final M14 mobile inventory instead of owning independent mass.");

    const auto finalPhysical =
        result.material.QueryMass(
            fixture.materials);

    Require(
        NearlyEqual(
            finalPhysical.LooseMassKg(),
            balance.finalLooseMassKg,
            physicalTolerance) &&
        NearlyEqual(
            std::max(
                finalPhysical.excavatedBedrockKg -
                    initialPhysical.excavatedBedrockKg,
                0.0),
            balance.abradedBedrockMassKg,
            physicalTolerance),
        "M30-05 independent M08 mass accounting must agree with M13 loose-material and bedrock-abrasion totals.");

    const auto& leeState =
        result.At(
            5U,
            resolution / 2U);
    const auto& windwardState =
        result.At(
            3U,
            resolution / 2U);

    Require(
        leeState.exposure <
            windwardState.exposure * 0.5F &&
        result.material.At(
            5U,
            resolution / 2U).
            sandMeters >
            initialSandMeters + 0.01F,
        "M30-05 conservation fixture must retain its intended lee-side deposition so mass closure is tested across real pickup/transport/deposition behavior.");
}


void Test06ThermalReposeConvergence()
{
    using namespace terrain_erosion;
    using namespace terrain_material_column;

    constexpr u32 resolution = 5U;
    constexpr f64 spacingMeters = 1.0;
    constexpr f32 initialSandMeters = 2.0F;

    StratigraphyFixture fixture;

    MaterialColumnPage page(
        resolution,
        spacingMeters);

    std::vector<f32> originalBedrock;
    originalBedrock.reserve(
        static_cast<std::size_t>(
            resolution) *
        resolution);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const f32 bedrockHeight =
                x == resolution / 2U &&
                y == resolution / 2U
                    ? 3.0F
                    : 0.0F;

            page.SetCell(
                x,
                y,
                {
                    .bedrockHeightMeters =
                        bedrockHeight,
                    .referenceBedrockHeightMeters =
                        bedrockHeight,
                    .bedrockMaterial =
                        terrain_geology::
                            reference_rock::
                                Basalt,
                    .regolithMeters = 0.0F,
                    .soilMeters = 0.0F,
                    .sandMeters =
                        initialSandMeters,
                    .debrisMeters = 0.0F,
                    .moisture = 0.0F,
                    .temporaryScalar = 0.0F
                });

            originalBedrock.push_back(
                bedrockHeight);
        }
    }

    const auto initialMass =
        page.QueryMass(
            fixture.materials);

    const auto maximumSandSlopeDegrees =
        [](const MaterialColumnPage& material)
        {
            constexpr f64 radiansToDegrees =
                57.2957795130823208768;

            f64 maximum = 0.0;

            for (u32 y = 0U;
                 y < material.Resolution();
                 ++y)
            {
                for (u32 x = 0U;
                     x < material.Resolution();
                     ++x)
                {
                    const auto& source =
                        material.At(x, y);

                    if (source.ExposedSurface() !=
                        ExposedSurfaceKind::Sand)
                    {
                        continue;
                    }

                    for (i32 dy = -1;
                         dy <= 1;
                         ++dy)
                    {
                        for (i32 dx = -1;
                             dx <= 1;
                             ++dx)
                        {
                            if (dx == 0 &&
                                dy == 0)
                            {
                                continue;
                            }

                            const i32 nx =
                                static_cast<i32>(x) +
                                dx;
                            const i32 ny =
                                static_cast<i32>(y) +
                                dy;

                            if (nx < 0 ||
                                ny < 0 ||
                                nx >=
                                    static_cast<i32>(
                                        material.Resolution()) ||
                                ny >=
                                    static_cast<i32>(
                                        material.Resolution()))
                            {
                                continue;
                            }

                            const f64 drop =
                                static_cast<f64>(
                                    source.
                                        SurfaceHeightMeters()) -
                                static_cast<f64>(
                                    material.At(
                                        static_cast<u32>(nx),
                                        static_cast<u32>(ny)).
                                        SurfaceHeightMeters());

                            if (drop <= 0.0)
                            {
                                continue;
                            }

                            const bool diagonal =
                                dx != 0 &&
                                dy != 0;

                            const f64 distance =
                                material.SpacingMeters() *
                                (diagonal
                                     ? 1.4142135623730951
                                     : 1.0);

                            maximum =
                                std::max(
                                    maximum,
                                    std::atan2(
                                        drop,
                                        distance) *
                                        radiansToDegrees);
                        }
                    }
                }
            }

            return maximum;
        };

    const f64 initialMaximumSlope =
        maximumSandSlopeDegrees(page);

    ThermalErosionConfig config{};
    config.maximumIterations = 128U;
    config.sandReposeDegrees = 33.0;
    config.bedrockFractureRate = 0.0;
    config.maximumTransferDepthPerIterationMeters =
        0.20;
    config.convergenceDepthMeters =
        2.0e-5;

    const auto first =
        SimulateThermalErosion(
            page,
            fixture.materials,
            {},
            config);

    Require(
        initialMaximumSlope >
            config.sandReposeDegrees +
                20.0,
        "M30-06 fixture must begin substantially above the sand angle of repose.");

    Require(
        first.converged &&
        first.iterationsExecuted > 1U &&
        first.iterationsExecuted <=
            config.maximumIterations,
        "M30-06 M12 must converge a genuinely unstable slope within its bounded local iteration budget.");

    f64 movedOutKg = 0.0;
    f64 receivedKg = 0.0;

    for (const auto& state :
         first.cells)
    {
        movedOutKg +=
            state.movedOutKg;
        receivedKg +=
            state.receivedKg;
    }

    Require(
        movedOutKg > 0.0 &&
        NearlyEqual(
            movedOutKg,
            receivedKg,
            std::max(
                movedOutKg * 1.0e-12,
                1.0e-6)),
        "M30-06 thermal relaxation must perform real internal mass transfer with equal moved/received ledgers.");

    const f64 finalMaximumSlope =
        maximumSandSlopeDegrees(
            first.material);

    Require(
        finalMaximumSlope <=
            config.sandReposeDegrees +
                0.02,
        "M30-06 converged exposed sand must remain at or below the configured repose angle within the convergence-depth tolerance.");

    const auto finalMass =
        first.material.QueryMass(
            fixture.materials);

    const f64 massReference =
        std::max(
            initialMass.LooseMassKg(),
            1.0);

    Require(
        NearlyEqual(
            first.massBalance.initialLooseMassKg,
            initialMass.LooseMassKg(),
            1.0e-6) &&
        NearlyEqual(
            first.massBalance.finalLooseMassKg,
            finalMass.LooseMassKg(),
            1.0e-6) &&
        NearlyEqual(
            first.massBalance.fracturedBedrockMassKg,
            0.0,
            1.0e-9) &&
        first.massBalance.materialBalanceRelativeError <
            2.0e-6 &&
        std::abs(
            first.massBalance.materialBalanceErrorKg) <=
            massReference *
                2.0e-6,
        "M30-06 loose thermal redistribution must conserve M08 mass without inventing fractured bedrock when fracture is disabled.");

    std::size_t bedrockIndex = 0U;

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& cell =
                first.material.At(x, y);

            Require(
                cell.bedrockHeightMeters ==
                    originalBedrock[bedrockIndex] &&
                cell.referenceBedrockHeightMeters ==
                    originalBedrock[bedrockIndex] &&
                cell.bedrockMaterial ==
                    terrain_geology::
                        reference_rock::
                            Basalt,
                "M30-06 loose-material convergence must not smooth, excavate or re-identify the underlying M08/M02 bedrock.");

            ++bedrockIndex;
        }
    }

    const auto replay =
        SimulateThermalErosion(
            first.material,
            fixture.materials,
            {},
            config);

    Require(
        replay.converged &&
        replay.iterationsExecuted == 1U,
        "M30-06 a converged thermal surface must be recognized as converged immediately on the next identical solve.");

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& before =
                first.material.At(x, y);
            const auto& after =
                replay.material.At(x, y);

            Require(
                before.bedrockHeightMeters ==
                        after.bedrockHeightMeters &&
                    before.referenceBedrockHeightMeters ==
                        after.referenceBedrockHeightMeters &&
                    before.regolithMeters ==
                        after.regolithMeters &&
                    before.soilMeters ==
                        after.soilMeters &&
                    before.sandMeters ==
                        after.sandMeters &&
                    before.debrisMeters ==
                        after.debrisMeters,
                "M30-06 converged thermal state must be an exact physical no-op on immediate replay.");
        }
    }
}


void Test07CrossPageWaterFlux()
{
    using namespace terrain_region;

    constexpr u32 resolution = 4U;
    constexpr u64 revision = 0x4D33305741544552ULL;

    const world::PlanetId planet{
        .high = 0x4F524249544D3330ULL,
        .low = 0x0000000000007000ULL
    };

    const terrain::PhysicalTerrainPageAddress sourceAddress{
        .planet = planet,
        .tile = {
            .face = world::CubeFace::PositiveX,
            .level = 2U,
            .x = 3U,
            .y = 1U
        }
    };

    const auto eastMapping =
        world::NeighborAcrossTileEdge(
            sourceAddress.tile,
            world::TileEdge::East);

    Require(
        eastMapping.tile.face !=
            sourceAddress.tile.face,
        "M30-07 fixture must cross a cube-face boundary rather than only a same-face page edge.");

    auto sourceFlux =
        MakeSurfaceBoundaryFlux(
            resolution,
            revision);

    f64 expectedEdgeVolume = 0.0;

    for (u32 sourceIndex = 0U;
         sourceIndex < resolution;
         ++sourceIndex)
    {
        const f64 volume =
            static_cast<f64>(
                sourceIndex + 1U);

        // Positive x is outward through the source east edge. The unique
        // tangent component makes sample reversal/vector remapping observable.
        sourceFlux.east[sourceIndex] = {
            .volumeCubicMeters = volume,
            .velocityMoment = {
                volume * 2.0,
                static_cast<f64>(
                    sourceIndex) +
                    0.25
            }
        };

        expectedEdgeVolume +=
            volume;
    }

    constexpr f64 cornerVolume = 5.5;

    sourceFlux.corners[
        static_cast<std::size_t>(
            SurfaceBoundaryCorner::
                NorthEast)] = {
        .volumeCubicMeters =
            cornerVolume,
        .velocityMoment = {
            4.0,
            -3.0
        }
    };

    const PhysicalPageBoundaryFlux source{
        .address = sourceAddress,
        .outgoing = sourceFlux
    };

    const terrain::PhysicalTerrainPageAddress quietAddress{
        .planet = planet,
        .tile = {
            .face = world::CubeFace::PositiveX,
            .level = 2U,
            .x = 1U,
            .y = 2U
        }
    };

    const PhysicalPageBoundaryFlux quiet{
        .address = quietAddress,
        .outgoing =
            MakeSurfaceBoundaryFlux(
                resolution,
                revision + 1U)
    };

    const std::array<PhysicalPageBoundaryFlux, 2> forward{
        source,
        quiet
    };

    const std::array<PhysicalPageBoundaryFlux, 2> reverse{
        quiet,
        source
    };

    const auto forwardBatch =
        BuildDeterministicBoundaryTransfers(
            forward);

    const auto reverseBatch =
        BuildDeterministicBoundaryTransfers(
            reverse);

    const f64 expectedTotalVolume =
        expectedEdgeVolume +
        cornerVolume;

    Require(
        NearlyEqual(
            forwardBatch.
                TotalWaterVolumeCubicMeters(),
            expectedTotalVolume,
            1.0e-12) &&
        NearlyEqual(
            reverseBatch.
                TotalWaterVolumeCubicMeters(),
            expectedTotalVolume,
            1.0e-12),
        "M30-07 M25 boundary construction must conserve every exported water volume independent of input page ordering.");

    Require(
        forwardBatch.edges.size() ==
            reverseBatch.edges.size() &&
        forwardBatch.corners.size() ==
            reverseBatch.corners.size(),
        "M30-07 deterministic water transfer batches must have order-independent edge/corner cardinality.");

    for (std::size_t index = 0U;
         index < forwardBatch.edges.size();
         ++index)
    {
        const auto& a =
            forwardBatch.edges[index];
        const auto& b =
            reverseBatch.edges[index];

        Require(
            a.source == b.source &&
            a.target == b.target &&
            a.sourceEdge == b.sourceEdge &&
            a.targetEdge == b.targetEdge &&
            a.reversed == b.reversed &&
            a.revision == b.revision &&
            a.water.size() == b.water.size(),
            "M30-07 deterministic edge routing changed when physical page input order was reversed.");

        for (std::size_t sample = 0U;
             sample < a.water.size();
             ++sample)
        {
            Require(
                NearlyEqual(
                    a.water[sample].
                        volumeCubicMeters,
                    b.water[sample].
                        volumeCubicMeters,
                    0.0) &&
                NearlyEqual(
                    a.water[sample].
                        velocityMoment.x,
                    b.water[sample].
                        velocityMoment.x,
                    0.0) &&
                NearlyEqual(
                    a.water[sample].
                        velocityMoment.y,
                    b.water[sample].
                        velocityMoment.y,
                    0.0),
                "M30-07 deterministic edge water packets changed with scheduler input order.");
        }
    }

    for (std::size_t index = 0U;
         index < forwardBatch.corners.size();
         ++index)
    {
        const auto& a =
            forwardBatch.corners[index];
        const auto& b =
            reverseBatch.corners[index];

        Require(
            a.source == b.source &&
            a.target == b.target &&
            a.sourceCorner == b.sourceCorner &&
            a.targetCorner == b.targetCorner &&
            a.revision == b.revision &&
            NearlyEqual(
                a.water.volumeCubicMeters,
                b.water.volumeCubicMeters,
                0.0) &&
            NearlyEqual(
                a.water.velocityMoment.x,
                b.water.velocityMoment.x,
                0.0) &&
            NearlyEqual(
                a.water.velocityMoment.y,
                b.water.velocityMoment.y,
                0.0),
            "M30-07 deterministic corner water routing changed with scheduler input order.");
    }

    const auto edgeTransfer =
        std::find_if(
            forwardBatch.edges.begin(),
            forwardBatch.edges.end(),
            [&](const SurfaceBoundaryEdgeTransfer& transfer)
            {
                return
                    transfer.source ==
                        sourceAddress &&
                    transfer.sourceEdge ==
                        world::TileEdge::East;
            });

    Require(
        edgeTransfer !=
            forwardBatch.edges.end(),
        "M30-07 cross-face east water transfer is missing.");

    Require(
        edgeTransfer->target.tile ==
                eastMapping.tile &&
        edgeTransfer->targetEdge ==
                eastMapping.edge &&
        edgeTransfer->reversed ==
                eastMapping.reverseSamples,
        "M30-07 water transfer must use the canonical M01 cube-face neighbor mapping.");

    for (u32 sourceIndex = 0U;
         sourceIndex < resolution;
         ++sourceIndex)
    {
        const u32 targetIndex =
            world::RemapTileEdgeSampleIndex(
                eastMapping,
                sourceIndex,
                resolution);

        const auto& packet =
            edgeTransfer->
                water[targetIndex];

        const f64 volume =
            static_cast<f64>(
                sourceIndex + 1U);

        const math::Double2 originalMoment{
            volume * 2.0,
            static_cast<f64>(
                sourceIndex) +
                0.25
        };

        const auto expectedMoment =
            TransformBoundaryVectorAcrossEdge(
                world::TileEdge::East,
                eastMapping,
                originalMoment);

        Require(
            NearlyEqual(
                packet.volumeCubicMeters,
                volume,
                0.0) &&
            NearlyEqual(
                packet.velocityMoment.x,
                expectedMoment.x,
                1.0e-12) &&
            NearlyEqual(
                packet.velocityMoment.y,
                expectedMoment.y,
                1.0e-12),
            "M30-07 cube-face water sample remap must preserve volume and rotate the conservative velocity moment into receiver-local orientation.");
    }

    const terrain::PhysicalTerrainPageAddress edgeReceiver{
        .planet = planet,
        .tile = eastMapping.tile
    };

    const auto incomingEdge =
        IncomingWaterBoundaryFlux(
            edgeReceiver,
            forwardBatch);

    Require(
        NearlyEqual(
            incomingEdge.volumeCubicMeters,
            expectedEdgeVolume,
            1.0e-12),
        "M30-07 neighboring physical page must receive exactly the water volume exported through the source edge.");

    const math::Double2 outward =
        TransformBoundaryVectorAcrossEdge(
            world::TileEdge::East,
            eastMapping,
            {1.0, 0.0});

    math::Double2 expectedInward{};

    switch (eastMapping.edge)
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

    Require(
        NearlyEqual(
            outward.x,
            expectedInward.x,
            1.0e-12) &&
        NearlyEqual(
            outward.y,
            expectedInward.y,
            1.0e-12),
        "M30-07 outward source water momentum must become inward receiver-local momentum across a physical page boundary.");

    const world::PlanetTileId cornerTile =
        world::OffsetTile(
            sourceAddress.tile,
            1,
            -1);

    const terrain::PhysicalTerrainPageAddress cornerReceiver{
        .planet = planet,
        .tile = cornerTile
    };

    const auto incomingCorner =
        IncomingWaterBoundaryFlux(
            cornerReceiver,
            forwardBatch);

    Require(
        NearlyEqual(
            incomingCorner.volumeCubicMeters,
            cornerVolume,
            1.0e-12),
        "M30-07 diagonal physical neighbor must receive exactly the exported corner water packet without edge duplication or loss.");

    const auto cornerTransfer =
        std::find_if(
            forwardBatch.corners.begin(),
            forwardBatch.corners.end(),
            [&](const SurfaceBoundaryCornerTransfer& transfer)
            {
                return
                    transfer.source ==
                        sourceAddress &&
                    transfer.sourceCorner ==
                        SurfaceBoundaryCorner::
                            NorthEast;
            });

    Require(
        cornerTransfer !=
            forwardBatch.corners.end() &&
        cornerTransfer->target ==
            cornerReceiver &&
        NearlyEqual(
            cornerTransfer->
                water.volumeCubicMeters,
            cornerVolume,
            0.0) &&
        std::isfinite(
            cornerTransfer->
                water.velocityMoment.x) &&
        std::isfinite(
            cornerTransfer->
                water.velocityMoment.y),
        "M30-07 corner water flux must route through the canonical physical diagonal neighbor with finite transformed momentum.");
}


void Test08CrossPageSedimentFlux()
{
    using namespace terrain_erosion;
    using namespace terrain_region;

    constexpr u32 resolution = 4U;
    constexpr f64 spacingMeters = 5.0;
    constexpr u64 revision = 0x4D3330534544494DULL;

    const world::PlanetId planet{
        .high = 0x4F524249544D3330ULL,
        .low = 0x0000000000008000ULL
    };

    const terrain::PhysicalTerrainPageAddress sourceAddress{
        .planet = planet,
        .tile = {
            .face = world::CubeFace::PositiveX,
            .level = 2U,
            .x = 3U,
            .y = 1U
        }
    };

    const auto mapping =
        world::NeighborAcrossTileEdge(
            sourceAddress.tile,
            world::TileEdge::East);

    Require(
        mapping.tile.face !=
            sourceAddress.tile.face,
        "M30-08 fixture must exercise sediment transfer across a cube-face boundary.");

    SedimentExchangePage sourceSediment(
        resolution,
        spacingMeters);

    const std::array<SedimentMass, 3> payloads{{
        {
            .sandKg = 1.0,
            .finesKg = 2.0,
            .coarseDebrisKg = 3.0
        },
        {
            .sandKg = 4.0,
            .finesKg = 5.0,
            .coarseDebrisKg = 6.0
        },
        {
            .sandKg = 7.0,
            .finesKg = 8.0,
            .coarseDebrisKg = 9.0
        }
    }};

    const std::array<SedimentTransportMedium, 3> media{{
        SedimentTransportMedium::Waterborne,
        SedimentTransportMedium::Airborne,
        SedimentTransportMedium::SurfaceMobile
    }};

    SedimentMass totalPayload{};

    for (std::size_t lane = 0U;
         lane < media.size();
         ++lane)
    {
        const u32 sourceY =
            static_cast<u32>(lane);

        sourceSediment.Add(
            resolution - 1U,
            sourceY,
            media[lane],
            payloads[lane]);

        const auto exported =
            sourceSediment.ExportAcrossBoundary(
                resolution - 1U,
                sourceY,
                static_cast<i32>(
                    resolution),
                static_cast<i32>(
                    sourceY),
                media[lane],
                payloads[lane]);

        Require(
            NearlyEqual(
                exported.sandKg,
                payloads[lane].sandKg,
                0.0) &&
            NearlyEqual(
                exported.finesKg,
                payloads[lane].finesKg,
                0.0) &&
            NearlyEqual(
                exported.coarseDebrisKg,
                payloads[lane].
                    coarseDebrisKg,
                0.0),
            "M30-08 source M14 page must export the exact requested typed sediment payload.");

        totalPayload +=
            payloads[lane];
    }

    Require(
        sourceSediment.
            TotalMobileMass().
            Empty(1.0e-12) &&
        NearlyEqual(
            sourceSediment.
                Accounting().
                exported.TotalKg(),
            totalPayload.TotalKg(),
            1.0e-12),
        "M30-08 full boundary export must remove the payload from source mobile state and record it exactly once as exported mass.");

    auto outgoing =
        sourceSediment.
            TakeOutgoingBoundaryFlux(
                revision);

    Require(
        NearlyEqual(
            outgoing.Total().sandKg,
            totalPayload.sandKg,
            1.0e-12) &&
        NearlyEqual(
            outgoing.Total().finesKg,
            totalPayload.finesKg,
            1.0e-12) &&
        NearlyEqual(
            outgoing.Total().
                coarseDebrisKg,
            totalPayload.
                coarseDebrisKg,
            1.0e-12),
        "M30-08 outgoing M14 boundary flux must preserve sediment classes before M25 remapping.");

    auto surface =
        MakeSurfaceBoundaryFlux(
            resolution,
            revision,
            std::move(outgoing));

    const PhysicalPageBoundaryFlux source{
        .address = sourceAddress,
        .outgoing = std::move(surface)
    };

    const auto batch =
        BuildDeterministicBoundaryTransfers(
            std::span<const PhysicalPageBoundaryFlux>(
                &source,
                1U));

    Require(
        NearlyEqual(
            batch.TotalSediment().sandKg,
            totalPayload.sandKg,
            1.0e-12) &&
        NearlyEqual(
            batch.TotalSediment().finesKg,
            totalPayload.finesKg,
            1.0e-12) &&
        NearlyEqual(
            batch.TotalSediment().
                coarseDebrisKg,
            totalPayload.
                coarseDebrisKg,
            1.0e-12),
        "M30-08 M25 remapping must conserve every typed sediment class.");

    const auto transfer =
        std::find_if(
            batch.edges.begin(),
            batch.edges.end(),
            [&](const SurfaceBoundaryEdgeTransfer& edge)
            {
                return
                    edge.source ==
                        sourceAddress &&
                    edge.sourceEdge ==
                        world::TileEdge::East;
            });

    Require(
        transfer !=
            batch.edges.end() &&
        transfer->target.tile ==
            mapping.tile &&
        transfer->targetEdge ==
            mapping.edge &&
        transfer->reversed ==
            mapping.reverseSamples,
        "M30-08 sediment edge transfer must use the canonical M01 cube-face mapping.");

    const auto receiverCoordinate =
        [](const world::TileEdge edge,
           const u32 sample)
        {
            switch (edge)
            {
            case world::TileEdge::North:
                return std::pair<u32, u32>{
                    sample,
                    0U};
            case world::TileEdge::East:
                return std::pair<u32, u32>{
                    resolution - 1U,
                    sample};
            case world::TileEdge::South:
                return std::pair<u32, u32>{
                    sample,
                    resolution - 1U};
            case world::TileEdge::West:
                return std::pair<u32, u32>{
                    0U,
                    sample};
            }

            return std::pair<u32, u32>{
                0U,
                0U};
        };

    for (std::size_t lane = 0U;
         lane < media.size();
         ++lane)
    {
        const u32 sourceY =
            static_cast<u32>(lane);

        const u32 targetIndex =
            world::RemapTileEdgeSampleIndex(
                mapping,
                sourceY,
                resolution);

        const auto& packet =
            transfer->
                sediment[targetIndex];

        const auto& mappedMass =
            packet.Medium(
                media[lane]);

        Require(
            NearlyEqual(
                mappedMass.sandKg,
                payloads[lane].sandKg,
                0.0) &&
            NearlyEqual(
                mappedMass.finesKg,
                payloads[lane].finesKg,
                0.0) &&
            NearlyEqual(
                mappedMass.coarseDebrisKg,
                payloads[lane].
                    coarseDebrisKg,
                0.0),
            "M30-08 sample remapping must preserve typed mass in its original M14 transport medium.");

        for (const auto other :
             media)
        {
            if (other ==
                media[lane])
            {
                continue;
            }

            Require(
                packet.Medium(other).
                    Empty(1.0e-12),
                "M30-08 cross-page remapping must not leak sediment into another transport medium.");
        }

        const f64 kilograms =
            payloads[lane].
                TotalKg();

        const auto transformed =
            TransformBoundaryVectorAcrossEdge(
                world::TileEdge::East,
                mapping,
                {
                    kilograms,
                    0.0
                });

        const auto& transport =
            packet.Transport(
                media[lane]);

        Require(
            NearlyEqual(
                transport.eastKg,
                transformed.x,
                1.0e-12) &&
            NearlyEqual(
                transport.northKg,
                -transformed.y,
                1.0e-12),
            "M30-08 M14 transport diagnostics must rotate from source-local east/north into receiver-local orientation with the packet.");
    }

    const terrain::PhysicalTerrainPageAddress receiverAddress{
        .planet = planet,
        .tile = mapping.tile
    };

    SedimentExchangePage receiverSediment(
        resolution,
        spacingMeters);

    ApplySedimentBoundaryTransfers(
        receiverAddress,
        batch,
        receiverSediment);

    const auto imported =
        receiverSediment.
            Accounting().
            imported;

    Require(
        NearlyEqual(
            imported.sandKg,
            totalPayload.sandKg,
            1.0e-12) &&
        NearlyEqual(
            imported.finesKg,
            totalPayload.finesKg,
            1.0e-12) &&
        NearlyEqual(
            imported.coarseDebrisKg,
            totalPayload.
                coarseDebrisKg,
            1.0e-12) &&
        NearlyEqual(
            receiverSediment.
                TotalMobileMass().
                TotalKg(),
            totalPayload.TotalKg(),
            1.0e-12),
        "M30-08 receiver M14 import accounting and mobile inventory must equal the source export exactly.");

    for (std::size_t lane = 0U;
         lane < media.size();
         ++lane)
    {
        const u32 targetIndex =
            world::RemapTileEdgeSampleIndex(
                mapping,
                static_cast<u32>(lane),
                resolution);

        const auto [x, y] =
            receiverCoordinate(
                mapping.edge,
                targetIndex);

        const auto& receiverCell =
            receiverSediment.At(
                x,
                y);

        const auto& received =
            receiverCell.Medium(
                media[lane]);

        Require(
            NearlyEqual(
                received.sandKg,
                payloads[lane].sandKg,
                0.0) &&
            NearlyEqual(
                received.finesKg,
                payloads[lane].finesKg,
                0.0) &&
            NearlyEqual(
                received.coarseDebrisKg,
                payloads[lane].
                    coarseDebrisKg,
                0.0),
            "M30-08 imported typed sediment must land on the receiver boundary cell selected by the canonical sample remap.");

        const f64 kilograms =
            payloads[lane].
                TotalKg();

        const auto transformed =
            TransformBoundaryVectorAcrossEdge(
                world::TileEdge::East,
                mapping,
                {
                    kilograms,
                    0.0
                });

        const auto& diagnostic =
            receiverSediment.
                TransportAt(
                    x,
                    y).
                Medium(
                    media[lane]);

        Require(
            NearlyEqual(
                diagnostic.eastKg,
                transformed.x,
                1.0e-12) &&
            NearlyEqual(
                diagnostic.northKg,
                -transformed.y,
                1.0e-12),
            "M30-08 receiver transport diagnostics must preserve the receiver-local direction produced by M25 remapping.");
    }

    Require(
        NearlyEqual(
            sourceSediment.
                Accounting().
                exported.TotalKg(),
            receiverSediment.
                Accounting().
                imported.TotalKg(),
            1.0e-12) &&
        NearlyEqual(
            sourceSediment.
                    TotalMobileMass().
                    TotalKg() +
                receiverSediment.
                    TotalMobileMass().
                    TotalKg(),
            totalPayload.TotalKg(),
            1.0e-12),
        "M30-08 two-page M14 mass must be conserved exactly across export, M25 remap and receiver import.");
}


void Test09CrossPageDuneMigration()
{
    using namespace terrain_erosion;
    using namespace terrain_material_column;
    using namespace terrain_region;

    constexpr u32 resolution = 7U;
    constexpr f64 spacingMeters = 5.0;
    constexpr f32 sourceSandMeters = 0.25F;
    constexpr u64 revision = 0x4D333044554E4539ULL;

    StratigraphyFixture fixture;

    const world::PlanetId planet{
        .high = 0x4F524249544D3330ULL,
        .low = 0x0000000000009000ULL
    };

    const terrain::PhysicalTerrainPageAddress sourceAddress{
        .planet = planet,
        .tile = {
            .face = world::CubeFace::PositiveX,
            .level = 3U,
            .x = 3U,
            .y = 3U
        }
    };

    const auto eastMapping =
        world::NeighborAcrossTileEdge(
            sourceAddress.tile,
            world::TileEdge::East);

    Require(
        eastMapping.tile.face ==
            sourceAddress.tile.face,
        "M30-09 fixture intentionally uses a same-face neighbor so the test isolates process-driven dune migration from cube-face orientation already covered by M30-08.");

    MaterialColumnPage sourcePage(
        resolution,
        spacingMeters);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            sourcePage.SetCell(
                x,
                y,
                {
                    .bedrockHeightMeters = 0.0F,
                    .referenceBedrockHeightMeters = 0.0F,
                    .bedrockMaterial =
                        terrain_geology::
                            reference_rock::
                                Basalt,
                    .regolithMeters = 0.0F,
                    .soilMeters = 0.0F,
                    .sandMeters =
                        x >= resolution - 2U
                            ? sourceSandMeters
                            : 0.0F,
                    .debrisMeters = 0.0F,
                    .moisture = 0.0F,
                    .temporaryScalar = 0.0F
                });
        }
    }

    const auto sourceInitialMass =
        sourcePage.QueryMass(
            fixture.materials);

    std::vector<AeolianCellForcing> sourceWind(
        static_cast<std::size_t>(
            resolution) *
            resolution,
        AeolianCellForcing{
            .windEastMetersPerSecond = 14.0F,
            .windNorthMetersPerSecond = 0.0F,
            .surfaceResistance = 0.0F
        });

    AeolianErosionConfig sourceConfig{};
    sourceConfig.iterations = 40U;
    sourceConfig.timeStepSeconds = 0.20;
    sourceConfig.capacityCoefficient = 0.045;
    sourceConfig.windSpeedExponent = 2.0;
    sourceConfig.shadowRayCells = 4U;
    sourceConfig.shadowStrength = 8.0;
    sourceConfig.windwardExposureGain = 0.50;
    sourceConfig.minimumExposure = 0.05;
    sourceConfig.maximumExposure = 1.75;
    sourceConfig.pickupRatePerSecond = 2.0;
    sourceConfig.depositionRatePerSecond = 2.0;
    sourceConfig.reptationFraction = 0.25;
    sourceConfig.saltationRatePerSecond = 3.0;
    sourceConfig.referenceSaltationWindMetersPerSecond = 12.0;
    sourceConfig.maximumSandPickupDepthPerStepMeters = 0.03;
    sourceConfig.maximumSoilPickupDepthPerStepMeters = 0.0;
    sourceConfig.maximumDepositionDepthPerStepMeters = 0.05;
    sourceConfig.maximumAvalancheDepthPerStepMeters = 0.05;
    sourceConfig.bedrockAbrasionMetersPerSecondAtReferenceWind = 0.0;
    sourceConfig.maximumBedrockAbrasionDepthPerStepMeters = 0.0;

    auto sourceResult =
        SimulateAeolianErosion(
            std::move(sourcePage),
            fixture.materials,
            sourceWind,
            sourceConfig);

    Require(
        sourceResult.sedimentExchange.has_value(),
        "M30-09 source M13 solve must expose canonical M14 mobile state.");

    const f64 exportedKg =
        sourceResult.sedimentExchange->
            Accounting().
            exported.
            TotalKg();

    Require(
        exportedKg > 1.0 &&
        sourceResult.massBalance.
            boundaryLossKg > 1.0 &&
        NearlyEqual(
            exportedKg,
            sourceResult.massBalance.
                boundaryLossKg,
            1.0e-6),
        "M30-09 source dune must produce measurable M13-driven M14 boundary export.");

    const auto sourceExported =
        sourceResult.sedimentExchange->
            Accounting().
            exported;

    Require(
        sourceExported.finesKg <=
                1.0e-9 &&
        sourceExported.coarseDebrisKg <=
                1.0e-9 &&
        sourceExported.sandKg >
                1.0,
        "M30-09 source migration fixture must export sand only, so the downwind physical buildup is unambiguously dune material.");

    auto outgoing =
        sourceResult.sedimentExchange->
            TakeOutgoingBoundaryFlux(
                revision);

    Require(
        NearlyEqual(
            outgoing.Total().TotalKg(),
            exportedKg,
            1.0e-6),
        "M30-09 M13-produced boundary packets must carry the full exported dune mass into M25.");

    auto surface =
        MakeSurfaceBoundaryFlux(
            resolution,
            revision,
            std::move(outgoing));

    const PhysicalPageBoundaryFlux sourceBoundary{
        .address = sourceAddress,
        .outgoing = std::move(surface)
    };

    const auto transfers =
        BuildDeterministicBoundaryTransfers(
            std::span<const PhysicalPageBoundaryFlux>(
                &sourceBoundary,
                1U));

    const terrain::PhysicalTerrainPageAddress receiverAddress{
        .planet = planet,
        .tile = eastMapping.tile
    };

    SedimentExchangePage receiverSediment(
        resolution,
        spacingMeters);

    ApplySedimentBoundaryTransfers(
        receiverAddress,
        transfers,
        receiverSediment);

    Require(
        NearlyEqual(
            receiverSediment.
                Accounting().
                imported.TotalKg(),
            exportedKg,
            1.0e-6) &&
        NearlyEqual(
            receiverSediment.
                TotalMobileMass().
                TotalKg(),
            exportedKg,
            1.0e-6),
        "M30-09 M25 must deliver the complete process-generated dune load into the neighboring M14 page before the receiver aeolian solve.");

    MaterialColumnPage receiverPage(
        resolution,
        spacingMeters);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            receiverPage.SetCell(
                x,
                y,
                {
                    .bedrockHeightMeters = 0.0F,
                    .referenceBedrockHeightMeters = 0.0F,
                    .bedrockMaterial =
                        terrain_geology::
                            reference_rock::
                                Basalt,
                    .regolithMeters = 0.0F,
                    .soilMeters = 0.0F,
                    .sandMeters = 0.0F,
                    .debrisMeters = 0.0F,
                    .moisture = 0.0F,
                    .temporaryScalar = 0.0F
                });
        }
    }

    const auto receiverInitialMass =
        receiverPage.QueryMass(
            fixture.materials);

    std::vector<AeolianCellForcing> calmReceiver(
        static_cast<std::size_t>(
            resolution) *
            resolution,
        AeolianCellForcing{
            .windEastMetersPerSecond = 0.0F,
            .windNorthMetersPerSecond = 0.0F,
            .surfaceResistance = 0.0F
        });

    AeolianErosionConfig receiverConfig{};
    receiverConfig.iterations = 4U;
    receiverConfig.timeStepSeconds = 0.20;
    receiverConfig.capacityCoefficient = 0.045;
    receiverConfig.pickupRatePerSecond = 0.0;
    receiverConfig.depositionRatePerSecond = 5.0;
    receiverConfig.reptationFraction = 0.0;
    receiverConfig.saltationRatePerSecond = 0.0;
    receiverConfig.maximumSandPickupDepthPerStepMeters = 0.0;
    receiverConfig.maximumSoilPickupDepthPerStepMeters = 0.0;
    receiverConfig.maximumDepositionDepthPerStepMeters = 2.0;
    receiverConfig.maximumAvalancheDepthPerStepMeters = 0.05;
    receiverConfig.bedrockAbrasionMetersPerSecondAtReferenceWind = 0.0;
    receiverConfig.maximumBedrockAbrasionDepthPerStepMeters = 0.0;

    const auto receiverResult =
        SimulateAeolianErosion(
            std::move(receiverPage),
            fixture.materials,
            calmReceiver,
            receiverConfig,
            std::move(receiverSediment));

    Require(
        receiverResult.sedimentExchange.has_value() &&
        NearlyEqual(
            receiverResult.massBalance.
                initialMobileMassKg,
            exportedKg,
            1.0e-6),
        "M30-09 receiver M13 solve must continue from imported M14 mobile state instead of replacing it with a fresh page.");

    Require(
        NearlyEqual(
            receiverResult.sedimentExchange->
                Accounting().
                imported.TotalKg(),
            exportedKg,
            1.0e-6),
        "M30-09 receiver M13 continuation must preserve the M14 import ledger supplied by M25.");

    const auto receiverFinalMass =
        receiverResult.material.QueryMass(
            fixture.materials);

    Require(
        receiverFinalMass.LooseMassKg() >
            receiverInitialMass.LooseMassKg() +
                1.0 &&
        receiverResult.sedimentExchange->
            TotalMobileMass().
            Empty(1.0e-6) &&
        receiverResult.massBalance.
            boundaryLossKg <=
                1.0e-9,
        "M30-09 imported dune sediment must become physical M08 loose sand on the downwind page rather than disappearing or immediately leaving the receiver.");

    f64 receiverSandMassKg = 0.0;
    f64 receiverSandDepthMeters = 0.0;

    const f64 receiverArea =
        receiverResult.material.
            CellAreaSquareMeters();

    for (const auto& cell :
         receiverResult.material.Cells())
    {
        receiverSandDepthMeters +=
            cell.sandMeters;

        receiverSandMassKg +=
            static_cast<f64>(
                cell.sandMeters) *
            receiverArea *
            receiverResult.material.
                Densities().
                sandKgPerCubicMeter;
    }

    Require(
        receiverSandDepthMeters > 0.0 &&
        NearlyEqual(
            receiverSandMassKg,
            exportedKg,
            std::max(
                exportedKg * 3.0e-5,
                1.0e-3)),
        "M30-09 process-generated exported sand must reappear as the same physical dune mass on the receiver page.");

    Require(
        receiverResult.massBalance.
            materialBalanceRelativeError <
            3.0e-5,
        "M30-09 receiver aeolian continuation must conserve imported mobile mass while depositing the dune.");

    const f64 combinedFinalMass =
        sourceResult.massBalance.
            finalLooseMassKg +
        sourceResult.sedimentExchange->
            TotalMobileMass().
            TotalKg() +
        receiverResult.massBalance.
            finalLooseMassKg +
        receiverResult.sedimentExchange->
            TotalMobileMass().
            TotalKg();

    const f64 combinedInitialMass =
        sourceInitialMass.LooseMassKg() +
        receiverInitialMass.LooseMassKg();

    Require(
        NearlyEqual(
            combinedFinalMass,
            combinedInitialMass,
            std::max(
                combinedInitialMass *
                    4.0e-5,
                1.0e-3)),
        "M30-09 two-page dune migration must conserve the original physical sand across M13 pickup, M14 export, M25 transfer, M14 import and receiver M13 deposition.");
}


void Test10FastFlowDrainageReference()
{
    using namespace terrain_hydrology;
    using namespace terrain_material_column;

    constexpr u32 resolution = 7U;
    constexpr f64 spacingMeters = 10.0;
    constexpr u32 pitX = 2U;
    constexpr u32 pitY = 3U;

    MaterialColumnPage material(
        resolution,
        spacingMeters);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const f32 crossSlope =
                static_cast<f32>(
                    std::abs(
                        static_cast<i32>(y) -
                        static_cast<i32>(
                            resolution / 2U))) *
                0.75F;

            f32 height =
                100.0F -
                static_cast<f32>(x) *
                    3.0F +
                crossSlope;

            if (x == pitX &&
                y == pitY)
            {
                height = 60.0F;
            }

            material.SetCell(
                x,
                y,
                {
                    .bedrockHeightMeters =
                        height,
                    .referenceBedrockHeightMeters =
                        height,
                    .bedrockMaterial =
                        terrain_geology::
                            reference_rock::
                                Basalt,
                    .regolithMeters = 0.0F,
                    .soilMeters = 0.0F,
                    .sandMeters = 0.0F,
                    .debrisMeters = 0.0F,
                    .moisture = 0.0F,
                    .temporaryScalar = 0.0F
                });
        }
    }

    std::vector<DrainageCellInput> inputs(
        static_cast<std::size_t>(
            resolution) *
            resolution);

    f64 expectedTotalDischarge = 0.0;

    const f64 cellArea =
        material.CellAreaSquareMeters();

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(y) *
                    resolution +
                x;

            const f32 runoff =
                static_cast<f32>(
                    0.0001 *
                    static_cast<f64>(
                        1U +
                        ((x + 2U * y) %
                         5U)));

            inputs[index] = {
                .runoffMetersPerSecond =
                    runoff,
                .authoredDrainage = 0.0F,
                .outlet = false
            };

            expectedTotalDischarge +=
                static_cast<f64>(
                    runoff) *
                cellArea;
        }
    }

    const auto makeBoundary =
        [](const f32 height)
        {
            return DrainageBoundaryCell{
                .surfaceHeightMeters =
                    height,
                .conditionedHeightMeters =
                    height,
                .authoredDrainage = 0.0F,
                .drainageAreaSquareMeters =
                    0.0,
                .dischargeCubicMetersPerSecond =
                    0.0,
                .flowDx = 0,
                .flowDy = 0
            };
        };

    DrainagePageHalo halo{};
    halo.revision =
        0x4D33304641535446ULL;

    halo.north.assign(
        resolution,
        makeBoundary(200.0F));
    halo.south.assign(
        resolution,
        makeBoundary(200.0F));
    halo.west.assign(
        resolution,
        makeBoundary(200.0F));
    halo.east.assign(
        resolution,
        makeBoundary(60.0F));

    halo.corners = {
        makeBoundary(200.0F),
        makeBoundary(200.0F),
        makeBoundary(200.0F),
        makeBoundary(200.0F)
    };

    terrain::PhysicalTerrainPageKey key{};
    key.resolution = resolution;
    key.revisions.geology = 10U;
    key.revisions.processes = 20U;
    key.revisions.climate = 30U;
    key.revisions.authoring = 40U;

    DrainageRoutingConfig config{};
    config.depressionPolicy =
        DepressionRoutingPolicy::
            FillToBoundary;
    config.minimumDrainageDropMeters =
        0.10F;
    config.authoredGuidanceWeight =
        0.0F;

    const DrainagePage drainage =
        BuildDrainagePage(
            material,
            key,
            inputs,
            halo,
            config);

    const auto& pit =
        drainage.At(
            pitX,
            pitY);

    Require(
        NearlyEqual(
            pit.surfaceHeightMeters,
            60.0,
            1.0e-6) &&
        pit.depressionFillMeters >
            20.0F &&
        pit.drainageElevationMeters >
            pit.surfaceHeightMeters,
        "M30-10 FastFlow reference fixture must exercise priority-flood conditioning while preserving the original M08 surface.");

    const std::size_t cellCount =
        static_cast<std::size_t>(
            resolution) *
        resolution;

    std::vector<i32> downstream(
        cellCount,
        -1);

    std::vector<u32> remainingDonors(
        cellCount,
        0U);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const u32 index =
                y * resolution +
                x;

            const auto& cell =
                drainage.At(x, y);

            Require(
                cell.flow.HasDownstream(),
                "M30-10 conditioned catchment must give every physical cell a downstream route.");

            if (cell.flow.exitsPage)
            {
                continue;
            }

            const i32 tx =
                static_cast<i32>(x) +
                cell.flow.dx;
            const i32 ty =
                static_cast<i32>(y) +
                cell.flow.dy;

            Require(
                tx >= 0 &&
                ty >= 0 &&
                tx <
                    static_cast<i32>(
                        resolution) &&
                ty <
                    static_cast<i32>(
                        resolution),
                "M30-10 internal M09 route points outside the physical page without an exit flag.");

            const u32 target =
                static_cast<u32>(ty) *
                    resolution +
                static_cast<u32>(tx);

            downstream[index] =
                static_cast<i32>(
                    target);

            ++remainingDonors[target];

            Require(
                drainage.At(
                    static_cast<u32>(tx),
                    static_cast<u32>(ty)).
                    drainageElevationMeters <
                cell.drainageElevationMeters,
                "M30-10 M09 graph must be strictly downhill on the conditioned drainage surface.");
        }
    }

    std::vector<f64> referenceArea(
        cellCount,
        cellArea);

    std::vector<f64> referenceDischarge(
        cellCount,
        0.0);

    for (std::size_t index = 0U;
         index < cellCount;
         ++index)
    {
        referenceDischarge[index] =
            static_cast<f64>(
                inputs[index].
                    runoffMetersPerSecond) *
            cellArea;
    }

    std::deque<u32> leaves;

    for (u32 index = 0U;
         index <
             static_cast<u32>(
                 cellCount);
         ++index)
    {
        if (remainingDonors[index] ==
            0U)
        {
            leaves.push_back(index);
        }
    }

    u32 processed = 0U;

    while (!leaves.empty())
    {
        const u32 index =
            leaves.front();

        leaves.pop_front();
        ++processed;

        if (downstream[index] <
            0)
        {
            continue;
        }

        const u32 target =
            static_cast<u32>(
                downstream[index]);

        referenceArea[target] +=
            referenceArea[index];

        referenceDischarge[target] +=
            referenceDischarge[index];

        Require(
            remainingDonors[target] >
                0U,
            "M30-10 independent donor graph encountered an invalid dependency count.");

        --remainingDonors[target];

        if (remainingDonors[target] ==
            0U)
        {
            leaves.push_back(
                target);
        }
    }

    Require(
        processed ==
            static_cast<u32>(
                cellCount),
        "M30-10 independent donor-graph reference detected a cycle in the supposedly downhill M09 graph.");

    f64 exitArea = 0.0;
    f64 exitDischarge = 0.0;
    f64 maximumAccumulatedArea =
        0.0;

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const u32 index =
                y * resolution +
                x;

            const auto& cell =
                drainage.At(x, y);

            maximumAccumulatedArea =
                std::max(
                    maximumAccumulatedArea,
                    cell.
                        drainageAreaSquareMeters);

            Require(
                NearlyEqual(
                    cell.
                        drainageAreaSquareMeters,
                    referenceArea[index],
                    1.0e-9) &&
                NearlyEqual(
                    cell.
                        dischargeCubicMetersPerSecond,
                    referenceDischarge[index],
                    1.0e-12),
                "M30-10 production M09 accumulation must match the independent donor-graph FastFlow reference for every cell.");

            if (cell.flow.exitsPage)
            {
                exitArea +=
                    cell.
                        drainageAreaSquareMeters;

                exitDischarge +=
                    cell.
                        dischargeCubicMetersPerSecond;
            }
        }
    }

    const f64 expectedTotalArea =
        cellArea *
        static_cast<f64>(
            cellCount);

    Require(
        maximumAccumulatedArea >=
            cellArea * 10.0,
        "M30-10 fixture must contain meaningful multi-donor accumulation rather than only isolated single-cell exits.");

    Require(
        NearlyEqual(
            exitArea,
            expectedTotalArea,
            1.0e-9) &&
        NearlyEqual(
            exitDischarge,
            expectedTotalDischarge,
            1.0e-12),
        "M30-10 sum of all page-exit drainage area/discharge must equal the complete conditioned catchment input exactly.");

    const DrainagePage replay =
        BuildDrainagePage(
            material,
            key,
            inputs,
            halo,
            config);

    Require(
        drainage.Revision() ==
            replay.Revision(),
        "M30-10 identical physical drainage inputs must retain deterministic M09 revision identity.");

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& a =
                drainage.At(x, y);
            const auto& b =
                replay.At(x, y);

            Require(
                a.drainageElevationMeters ==
                        b.drainageElevationMeters &&
                    a.depressionFillMeters ==
                        b.depressionFillMeters &&
                    a.flow.dx ==
                        b.flow.dx &&
                    a.flow.dy ==
                        b.flow.dy &&
                    a.flow.exitsPage ==
                        b.flow.exitsPage &&
                    a.drainageAreaSquareMeters ==
                        b.drainageAreaSquareMeters &&
                    a.dischargeCubicMetersPerSecond ==
                        b.dischargeCubicMetersPerSecond,
                "M30-10 FastFlow drainage reference must be bit-deterministic for fixed physical inputs.");
        }
    }
}


void Test11AuthoredCanyonPersistence()
{
    using namespace surface_authoring;
    using namespace terrain_erosion;
    using namespace terrain_hydrology;
    using namespace terrain_macro_geology;
    using namespace terrain_material_column;

    constexpr u32 resolution = 7U;
    constexpr u32 centerY = resolution / 2U;

    StratigraphyFixture fixture;

    const world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0,
        .id = {
            .high = 0x4D333043414E594FULL,
            .low = 0x4E0000000000000BULL
        },
        .generationSeed =
            0x4D3330313143414EULL
    };

    terrain::PhysicalTerrainPageKey key{};
    key.address.planet =
        planet.id;
    key.address.tile = {
        .face = world::CubeFace::PositiveX,
        .level = 10U,
        .x = 511U,
        .y = 511U
    };
    key.resolution =
        resolution;
    key.revisions.geology = 11U;
    key.revisions.authoring = 17U;
    key.revisions.processes = 10U;
    key.revisions.climate = 3U;

    const world::CubeBounds bounds =
        world::TileBounds(
            key.address.tile);

    const auto pagePosition =
        [&](const u32 x,
            const u32 y)
        {
            const f64 tx =
                static_cast<f64>(x) /
                static_cast<f64>(
                    resolution - 1U);

            const f64 ty =
                static_cast<f64>(y) /
                static_cast<f64>(
                    resolution - 1U);

            const f64 u =
                bounds.minimumUv.x +
                (bounds.maximumUv.x -
                 bounds.minimumUv.x) *
                    tx;

            const f64 v =
                bounds.minimumUv.y +
                (bounds.maximumUv.y -
                 bounds.minimumUv.y) *
                    ty;

            return
                terrain::CanonicalizeSurfacePosition({
                    .planet = planet.id,
                    .unitDirection =
                        world::CubeToUnitDirection({
                            .face =
                                bounds.face,
                            .uv = {u, v}
                        }),
                    .radialOffsetMeters =
                        0.0
                });
        };

    const auto centerStart =
        pagePosition(
            0U,
            centerY);

    const auto centerNext =
        pagePosition(
            1U,
            centerY);

    const f64 cellSpacingMeters =
        std::acos(
            std::clamp(
                math::Dot(
                    centerStart.unitDirection,
                    centerNext.unitDirection),
                -1.0,
                1.0)) *
        planet.radiusMeters;

    Require(
        std::isfinite(
            cellSpacingMeters) &&
        cellSpacingMeters >
            100.0,
        "M30-11 physical page must resolve a meaningful authored canyon width.");

    MaterialColumnPage upstream(
        resolution,
        cellSpacingMeters);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const f32 awayFromCenter =
                static_cast<f32>(
                    std::abs(
                        static_cast<i32>(y) -
                        static_cast<i32>(
                            centerY)));

            const f32 height =
                220.0F -
                static_cast<f32>(x) *
                    1.5F -
                awayFromCenter *
                    1.0F;

            upstream.SetCell(
                x,
                y,
                {
                    .bedrockHeightMeters =
                        height,
                    .referenceBedrockHeightMeters =
                        height,
                    .bedrockMaterial =
                        terrain_geology::
                            reference_rock::
                                VolcanicAsh,
                    .regolithMeters = 0.0F,
                    .soilMeters = 0.0F,
                    .sandMeters = 0.0F,
                    .debrisMeters = 0.0F,
                    .moisture = 0.0F,
                    .temporaryScalar = 0.0F
                });
        }
    }

    const SplineConstraintPrimitive canyon{
        .controlUnitDirections = {
            pagePosition(
                0U,
                centerY).
                unitDirection,
            pagePosition(
                resolution / 2U,
                centerY).
                unitDirection,
            pagePosition(
                resolution - 1U,
                centerY).
                unitDirection
        },
        .halfWidthMeters =
            cellSpacingMeters *
            0.30,
        .falloffMeters =
            cellSpacingMeters *
            0.35
    };

    TerrainConstraintSet authored{
        .id = {
            .high = 0x4D333043414E594FULL,
            .low = 0x4E00000000000100ULL
        },
        .planet = planet.id,
        .name =
            "M30 authored canyon"
    };

    authored.height.constraints.push_back({
        .id = {
            .high = 0x4D333043414E594FULL,
            .low = 0x4E00000000000101ULL
        },
        .mode =
            ConstraintCompositionMode::Add,
        .primitive = canyon,
        .value = -24.0,
        .opacity = 1.0,
        .enabled = true
    });

    authored.protection.constraints.push_back({
        .id = {
            .high = 0x4D333043414E594FULL,
            .low = 0x4E00000000000102ULL
        },
        .mode =
            ConstraintCompositionMode::Replace,
        .primitive = canyon,
        .value = 0.60,
        .opacity = 1.0,
        .enabled = true
    });

    authored.drainage.constraints.push_back({
        .id = {
            .high = 0x4D333043414E594FULL,
            .low = 0x4E00000000000103ULL
        },
        .mode =
            ConstraintCompositionMode::Replace,
        .primitive = canyon,
        .value = 1.0,
        .opacity = 1.0,
        .enabled = true
    });

    Require(
        authored.IsValid(),
        "M30-11 authored spline canyon must be valid M04 project authority.");

    terrain::GlobalTerrainFieldDesc globalDesc{};
    globalDesc.seed =
        0x4D33303131474C42ULL;

    terrain::GlobalTerrainFields globals(
        planet,
        globalDesc);

    MacroGeologyDesc macroDesc{};
    macroDesc.seed =
        0x4D333031314D4143ULL;
    macroDesc.convergenceUpliftMeters =
        0.0;
    macroDesc.divergenceSubsidenceMeters =
        0.0;
    macroDesc.distortionAmplitude =
        0.0;
    macroDesc.distortionOctaves =
        1U;

    MacroGeologyField authoredMacro(
        planet,
        globals,
        &authored,
        macroDesc);

    MacroGeologyField controlMacro(
        planet,
        globals,
        nullptr,
        macroDesc);

    const auto authoredForcing =
        BuildStreamPowerForcing(
            upstream,
            key,
            planet,
            authoredMacro);

    const auto controlForcing =
        BuildStreamPowerForcing(
            upstream,
            key,
            planet,
            controlMacro);

    std::vector<DrainageCellInput>
        authoredDrainage(
            static_cast<std::size_t>(
                resolution) *
            resolution);

    std::vector<DrainageCellInput>
        controlDrainage(
            static_cast<std::size_t>(
                resolution) *
            resolution);

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(y) *
                    resolution +
                x;

            const auto authoredSample =
                authoredMacro.Sample(
                    pagePosition(
                        x,
                        y));

            const auto controlSample =
                controlMacro.Sample(
                    pagePosition(
                        x,
                        y));

            authoredDrainage[index] = {
                .runoffMetersPerSecond =
                    0.001F,
                .authoredDrainage =
                    static_cast<f32>(
                        authoredSample.
                            drainageGuidance),
                .outlet = false
            };

            controlDrainage[index] = {
                .runoffMetersPerSecond =
                    0.001F,
                .authoredDrainage =
                    static_cast<f32>(
                        controlSample.
                            drainageGuidance),
                .outlet = false
            };
        }
    }

    const std::size_t centerIndex =
        static_cast<std::size_t>(
            centerY) *
            resolution +
        resolution / 2U;

    const std::size_t shoulderIndex =
        static_cast<std::size_t>(
            centerY - 2U) *
            resolution +
        resolution / 2U;

    Require(
        authoredForcing[
            centerIndex].
            authoredElevationOffsetMeters <
                -23.0 &&
        authoredForcing[
            centerIndex].
            protection >
                0.59 &&
        authoredDrainage[
            centerIndex].
            authoredDrainage >
                0.99F &&
        std::abs(
            authoredForcing[
                shoulderIndex].
                authoredElevationOffsetMeters) <
                1.0e-3 &&
        authoredDrainage[
            shoulderIndex].
            authoredDrainage <
                1.0e-3F,
        "M30-11 M04 canyon height/protection/drainage fields must remain spatially coherent after M05 sampling.");

    Require(
        std::abs(
            controlForcing[
                centerIndex].
                authoredElevationOffsetMeters) <
                1.0e-9 &&
        controlForcing[
            centerIndex].
            protection <
                1.0e-9 &&
        controlDrainage[
            centerIndex].
            authoredDrainage <
                1.0e-9F,
        "M30-11 un-authored control must not contain hidden canyon state.");

    const auto boundary =
        [](const f32 height)
        {
            return DrainageBoundaryCell{
                .surfaceHeightMeters =
                    height,
                .conditionedHeightMeters =
                    height,
                .authoredDrainage = 0.0F,
                .drainageAreaSquareMeters =
                    0.0,
                .dischargeCubicMetersPerSecond =
                    0.0,
                .flowDx = 0,
                .flowDy = 0
            };
        };

    DrainagePageHalo halo{};
    halo.revision =
        0x4D333043414E4841ULL;
    halo.north.assign(
        resolution,
        boundary(400.0F));
    halo.south.assign(
        resolution,
        boundary(400.0F));
    halo.west.assign(
        resolution,
        boundary(400.0F));
    halo.east.assign(
        resolution,
        boundary(100.0F));
    halo.corners = {
        boundary(400.0F),
        boundary(400.0F),
        boundary(400.0F),
        boundary(400.0F)
    };

    StreamPowerErosionConfig config{};
    config.iterations = 8U;
    config.upliftCouplingPerIteration =
        0.0;
    config.authoredHeightRelaxation =
        0.50;
    config.incisionCoefficientMetersPerIteration =
        2.0;
    config.drainageExponent =
        0.5;
    config.slopeExponent =
        1.0;
    config.referenceDrainageAreaSquareMeters =
        upstream.CellAreaSquareMeters();
    config.referenceDischargeCubicMetersPerSecond =
        1.0;
    config.looseMaterialErodibility =
        1.0;
    config.minimumBedSlope =
        1.0e-6;
    config.maximumIncisionMetersPerIteration =
        2.0;
    config.drainage.depressionPolicy =
        DepressionRoutingPolicy::
            FillToBoundary;
    config.drainage.minimumDrainageDropMeters =
        0.01F;
    config.drainage.authoredGuidanceWeight =
        0.75F;

    const auto authoredResult =
        SolveStreamPowerErosion(
            upstream,
            key,
            fixture.materials,
            authoredDrainage,
            halo,
            authoredForcing,
            config);

    terrain::PhysicalTerrainPageKey controlKey =
        key;

    controlKey.revisions.authoring =
        0U;

    const auto controlResult =
        SolveStreamPowerErosion(
            upstream,
            controlKey,
            fixture.materials,
            controlDrainage,
            halo,
            controlForcing,
            config);

    const auto& authoredCenter =
        authoredResult.At(
            resolution / 2U,
            centerY);

    const auto& controlCenter =
        controlResult.At(
            resolution / 2U,
            centerY);

    const auto& authoredDownstream =
        authoredResult.At(
            resolution - 2U,
            centerY);

    const auto& controlDownstream =
        controlResult.At(
            resolution - 2U,
            centerY);

    Require(
        authoredCenter.
            finalSurfaceHeightMeters <
            controlCenter.
                finalSurfaceHeightMeters -
                10.0F,
        "M30-11 M04 canyon must remain a large-scale depression after iterative M09/M10 regeneration.");

    Require(
        authoredDownstream.
            finalDrainageAreaSquareMeters >
            controlDownstream.
                finalDrainageAreaSquareMeters +
                upstream.
                    CellAreaSquareMeters(),
        "M30-11 authored canyon must attract additional contributing area instead of remaining a cosmetic height edit.");

    Require(
        authoredCenter.
            cumulativeIncisionMeters >
                0.0F &&
        authoredCenter.
            cumulativeIncisionMeters <
            16.0F,
        "M30-11 canyon must participate in real M10 erosion while M04 protection keeps the authored feature from becoming an immutable stamp or unrestricted carve.");

    MaterialColumnPage baked =
        upstream;

    const auto bake =
        ApplyStreamPowerErosionResult(
            baked,
            fixture.materials,
            authoredResult);

    Require(
        bake.totalIncisionMeters >
            0.0 &&
        baked.At(
            resolution / 2U,
            centerY).
            SurfaceHeightMeters() <
            upstream.At(
                resolution / 2U,
                centerY).
                SurfaceHeightMeters() -
                10.0F,
        "M30-11 authored canyon must bake through the normal M10→M08 physical path.");

    MacroGeologyField regeneratedMacro(
        planet,
        globals,
        &authored,
        macroDesc);

    const auto regeneratedForcing =
        BuildStreamPowerForcing(
            upstream,
            key,
            planet,
            regeneratedMacro);

    std::vector<DrainageCellInput>
        regeneratedDrainage(
            authoredDrainage.size());

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const std::size_t index =
                static_cast<std::size_t>(y) *
                    resolution +
                x;

            regeneratedDrainage[index] = {
                .runoffMetersPerSecond =
                    0.001F,
                .authoredDrainage =
                    static_cast<f32>(
                        regeneratedMacro.
                            Sample(
                                pagePosition(
                                    x,
                                    y)).
                            drainageGuidance),
                .outlet = false
            };
        }
    }

    const auto regeneratedResult =
        SolveStreamPowerErosion(
            upstream,
            key,
            fixture.materials,
            regeneratedDrainage,
            halo,
            regeneratedForcing,
            config);

    Require(
        regeneratedResult.revision ==
            authoredResult.revision,
        "M30-11 regenerating derived terrain from unchanged authored authority must reproduce the same M10 revision identity.");

    for (u32 y = 0U;
         y < resolution;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution;
             ++x)
        {
            const auto& a =
                authoredResult.At(
                    x,
                    y);

            const auto& b =
                regeneratedResult.At(
                    x,
                    y);

            Require(
                a.finalSurfaceHeightMeters ==
                        b.finalSurfaceHeightMeters &&
                    a.cumulativeIncisionMeters ==
                        b.cumulativeIncisionMeters &&
                    a.finalDrainageAreaSquareMeters ==
                        b.finalDrainageAreaSquareMeters &&
                    a.finalDischargeCubicMetersPerSecond ==
                        b.finalDischargeCubicMetersPerSecond &&
                    a.finalFlowDx ==
                        b.finalFlowDx &&
                    a.finalFlowDy ==
                        b.finalFlowDy &&
                    a.finalFlowExitsPage ==
                        b.finalFlowExitsPage,
                "M30-11 authored canyon must survive complete derived-field regeneration bit-deterministically.");
        }
    }
}


void Test12BiomeFallback()
{
    using namespace terrain_biome;

    const universe::BodyId body{
        .high = 0x4D333042494F4D45ULL,
        .low = 0x0000000000000012ULL
    };

    BiomeService service(
        body);

    const BiomeId automaticId{
        .high = 0x4D333042494F4D45ULL,
        .low = 0x0000000000001201ULL
    };

    const BiomeId authoredId{
        .high = 0x4D333042494F4D45ULL,
        .low = 0x0000000000001202ULL
    };

    BiomeDefinition automatic{
        .id = automaticId,
        .name = "M30 hot biome",
        .placement = {
            .minimumResolvedWeight = 0.05F,
            .enabled = true,
            .mode =
                BiomePlacementMode::
                    Automatic,
            .selectors = {
                BiomeAutomaticSelector{
                    .field =
                        BiomeSelectorField::
                            Temperature,
                    .minimum = 35.0,
                    .maximum = 45.0,
                    .lowerFalloff = 0.0,
                    .upperFalloff = 0.0,
                    .invert = false,
                    .enabled = true
                }
            }
        }
    };

    BiomeDefinition authored{
        .id = authoredId,
        .name = "M30 authored biome",
        .placement = {
            .minimumResolvedWeight = 0.05F,
            .enabled = true,
            .mode =
                BiomePlacementMode::
                    Authored,
            .authoredMasks = {
                BiomeAuthoredMask{
                    .id = {
                        .high =
                            0x4D333042494F4D45ULL,
                        .low =
                            0x00000000000012A1ULL
                    },
                    .operation =
                        BiomeAuthoredWeightOperation::
                            Replace,
                    .centerUnitDirection =
                        {0.0, -1.0, 0.0},
                    .innerRadiusMeters =
                        1'000.0,
                    .outerRadiusMeters =
                        2'000.0,
                    .global = false,
                    .value = 1.0,
                    .opacity = 1.0,
                    .enabled = true
                }
            }
        }
    };

    service.UpsertBiome(
        automatic);

    service.UpsertBiome(
        authored);

    Require(
        service.Definitions().size() ==
            3U,
        "M30-12 fixture must contain exactly one BaseBiome and two optional biome definitions.");

    const auto baseId =
        service.BaseBiome().id;

    Require(
        baseId ==
            BiomeService::
                BaseBiomeId(body),
        "M30-12 BaseBiome identity must be the stable body-derived M19 fallback ID.");

    BiomePlacementContext basaltContext{};
    basaltContext.unitDirection =
        {0.0, 1.0, 0.0};
    basaltContext.planetRadiusMeters =
        6'000'000.0;
    basaltContext.temperatureC =
        15.0;
    basaltContext.moisture =
        0.5;
    basaltContext.rainfall =
        0.5;
    basaltContext.elevationMeters =
        250.0;
    basaltContext.slopeDegrees =
        5.0;
    basaltContext.aspectRadians =
        0.0;
    basaltContext.latitudeRadians =
        0.0;
    basaltContext.continentality =
        0.5;
    basaltContext.distanceToCoastWaterMeters =
        50'000.0;
    basaltContext.drainage =
        0.25;
    basaltContext.soilDepthMeters =
        0.5;
    basaltContext.sandDepthMeters =
        0.0;
    basaltContext.substrateRock =
        terrain_geology::
            reference_rock::
                Basalt;
    basaltContext.solarExposure =
        0.5;
    basaltContext.windExposure =
        0.5;
    basaltContext.snowPersistence =
        0.0;

    Require(
        basaltContext.IsValid(),
        "M30-12 fallback placement context must be valid.");

    const auto automaticEvaluation =
        service.EvaluatePlacement(
            automatic,
            basaltContext);

    const auto authoredEvaluation =
        service.EvaluatePlacement(
            authored,
            basaltContext);

    Require(
        NearlyEqual(
            automaticEvaluation.
                finalWeight,
            0.0,
            0.0) &&
        NearlyEqual(
            authoredEvaluation.
                finalWeight,
            0.0,
            0.0),
        "M30-12 optional automatic/authored selectors must contribute no coverage in the fallback fixture.");

    const auto basaltResolved =
        service.ResolvePlacement(
            basaltContext);

    Require(
        basaltResolved.size() ==
                1U &&
        basaltResolved.front().id ==
                baseId &&
        basaltResolved.front().base &&
        NearlyEqual(
            basaltResolved.front().
                weight,
            1.0,
            0.0),
        "M30-12 zero optional coverage must resolve to exactly one full-weight BaseBiome.");

    BiomePlacementContext graniteContext =
        basaltContext;

    graniteContext.substrateRock =
        terrain_geology::
            reference_rock::
                Granite;

    const auto graniteResolved =
        service.ResolvePlacement(
            graniteContext);

    Require(
        graniteResolved.size() ==
                1U &&
        graniteResolved.front().id ==
                baseId &&
        graniteResolved.front().base &&
        graniteResolved.front().weight ==
                basaltResolved.front().
                    weight,
        "M30-12 BaseBiome fallback must be independent of M02 substrate-rock identity when placement selectors contribute nothing.");

    const auto replay =
        service.ResolvePlacement(
            basaltContext);

    Require(
        replay.size() ==
                basaltResolved.size() &&
        replay.front().id ==
                basaltResolved.front().id &&
        replay.front().weight ==
                basaltResolved.front().weight &&
        replay.front().base ==
                basaltResolved.front().base,
        "M30-12 biome fallback must be bit-deterministic for fixed physical placement context.");

    const u64 revisionBeforeRemoval =
        service.Revision();

    Require(
        !service.RemoveBiome(
            baseId) &&
        service.Revision() ==
            revisionBeforeRemoval &&
        service.Find(baseId) !=
            nullptr,
        "M30-12 BaseBiome must be non-removable and a failed removal attempt must not mutate biome authority revision.");

    const BiomeId staleId{
        .high = 0x4D333042494F4D45ULL,
        .low = 0x00000000DEADBEEFULL
    };

    const std::array<
        BiomeWeightContribution,
        3>
        staleAndWeak{{
            {
                .id = staleId,
                .weight = 1.0F
            },
            {
                .id = automaticId,
                .weight = 0.01F
            },
            {
                .id = baseId,
                .weight = 1.0F
            }
        }};

    const auto staleResolved =
        service.Resolve(
            staleAndWeak);

    Require(
        staleResolved.size() ==
                1U &&
        staleResolved.front().id ==
                baseId &&
        staleResolved.front().base &&
        NearlyEqual(
            staleResolved.front().
                weight,
            1.0,
            0.0),
        "M30-12 stale IDs, below-threshold optional coverage and explicit BaseBiome contributions must all collapse safely to full fallback coverage.");
}


void Test13AutomaticAuthoredBiomeBlend()
{
    using namespace terrain_biome;

    const universe::BodyId body{
        .high = 0x4D333042494F4D45ULL,
        .low = 0x0000000000000013ULL
    };

    const BiomeId forestId{
        .high = 0x4D333042494F4D45ULL,
        .low = 0x0000000000001301ULL
    };

    const BiomeId duneId{
        .high = 0x4D333042494F4D45ULL,
        .low = 0x0000000000001302ULL
    };

    const BiomeId weakId{
        .high = 0x4D333042494F4D45ULL,
        .low = 0x0000000000001303ULL
    };

    const auto globalMask =
        [](const u64 low,
           const BiomeAuthoredWeightOperation operation,
           const f64 value)
        {
            return BiomeAuthoredMask{
                .id = {
                    .high =
                        0x4D3330424D41534BULL,
                    .low = low
                },
                .operation = operation,
                .centerUnitDirection =
                    {0.0, 1.0, 0.0},
                .innerRadiusMeters = 0.0,
                .outerRadiusMeters = 0.0,
                .global = true,
                .value = value,
                .opacity = 1.0,
                .enabled = true
            };
        };

    BiomeDefinition forest{
        .id = forestId,
        .name = "M30 blended forest",
        .placement = {
            .minimumResolvedWeight = 0.05F,
            .enabled = true,
            .mode =
                BiomePlacementMode::
                    AutomaticAndAuthored,
            .selectors = {
                BiomeAutomaticSelector{
                    .field =
                        BiomeSelectorField::
                            Temperature,
                    .minimum = 10.0,
                    .maximum = 20.0,
                    .lowerFalloff = 10.0,
                    .upperFalloff = 0.0,
                    .invert = false,
                    .enabled = true
                }
            },
            .authoredMasks = {
                globalMask(
                    0x1301ULL,
                    BiomeAuthoredWeightOperation::
                        Add,
                    0.125)
            }
        }
    };

    BiomeDefinition dunes{
        .id = duneId,
        .name = "M30 blended dunes",
        .placement = {
            .minimumResolvedWeight = 0.05F,
            .enabled = true,
            .mode =
                BiomePlacementMode::
                    AutomaticAndAuthored,
            .selectors = {
                BiomeAutomaticSelector{
                    .field =
                        BiomeSelectorField::
                            Moisture,
                    .minimum = 0.4,
                    .maximum = 0.8,
                    .lowerFalloff = 0.4,
                    .upperFalloff = 0.0,
                    .invert = false,
                    .enabled = true
                }
            },
            .authoredMasks = {
                globalMask(
                    0x1302ULL,
                    BiomeAuthoredWeightOperation::
                        Multiply,
                    0.5)
            }
        }
    };

    BiomeDefinition weak{
        .id = weakId,
        .name = "M30 thresholded biome",
        .placement = {
            .minimumResolvedWeight = 0.05F,
            .enabled = true,
            .mode =
                BiomePlacementMode::
                    AutomaticAndAuthored,
            .selectors = {
                BiomeAutomaticSelector{
                    .field =
                        BiomeSelectorField::
                            Temperature,
                    .minimum = 10.0,
                    .maximum = 20.0,
                    .lowerFalloff = 10.0,
                    .upperFalloff = 0.0,
                    .invert = false,
                    .enabled = true
                }
            },
            .authoredMasks = {
                globalMask(
                    0x1303ULL,
                    BiomeAuthoredWeightOperation::
                        Multiply,
                    0.05)
            }
        }
    };

    BiomeService service(
        body);

    // Insert in deliberately non-ID order. ResolvePlacement/Resolve must emit
    // optional biome entries in stable ID order rather than hash-map order.
    service.UpsertBiome(
        weak);
    service.UpsertBiome(
        dunes);
    service.UpsertBiome(
        forest);

    BiomePlacementContext context{};
    context.unitDirection =
        {0.0, 1.0, 0.0};
    context.planetRadiusMeters =
        6'000'000.0;
    context.temperatureC =
        5.0;
    context.moisture =
        0.2;
    context.rainfall =
        0.5;
    context.elevationMeters =
        200.0;
    context.slopeDegrees =
        8.0;
    context.aspectRadians =
        0.0;
    context.latitudeRadians =
        0.0;
    context.continentality =
        0.5;
    context.distanceToCoastWaterMeters =
        10'000.0;
    context.drainage =
        0.2;
    context.soilDepthMeters =
        0.5;
    context.sandDepthMeters =
        0.1;
    context.substrateRock =
        terrain_geology::
            reference_rock::
                Basalt;
    context.solarExposure =
        0.5;
    context.windExposure =
        0.5;
    context.snowPersistence =
        0.0;

    Require(
        context.IsValid(),
        "M30-13 biome blend context must be valid.");

    const auto forestEval =
        service.EvaluatePlacement(
            *service.Find(forestId),
            context);

    const auto duneEval =
        service.EvaluatePlacement(
            *service.Find(duneId),
            context);

    const auto weakEval =
        service.EvaluatePlacement(
            *service.Find(weakId),
            context);

    Require(
        NearlyEqual(
            forestEval.automaticWeight,
            0.5,
            1.0e-12) &&
        NearlyEqual(
            forestEval.authoredWeight,
            0.125,
            1.0e-12) &&
        NearlyEqual(
            forestEval.finalWeight,
            0.625,
            1.0e-12),
        "M30-13 automatic+authored Add composition must apply the authored mask after smooth automatic placement.");

    Require(
        NearlyEqual(
            duneEval.automaticWeight,
            0.5,
            1.0e-12) &&
        NearlyEqual(
            duneEval.finalWeight,
            0.25,
            1.0e-12),
        "M30-13 automatic+authored Multiply composition must scale the automatic biome weight.");

    Require(
        NearlyEqual(
            weakEval.automaticWeight,
            0.5,
            1.0e-12) &&
        NearlyEqual(
            weakEval.finalWeight,
            0.025,
            1.0e-12),
        "M30-13 threshold fixture must produce a nonzero final optional biome weight below its resolution threshold.");

    const auto resolved =
        service.ResolvePlacement(
            context);

    Require(
        resolved.size() ==
            3U,
        "M30-13 resolved blend must contain BaseBiome plus the two eligible optional biomes only.");

    Require(
        resolved[0].id ==
                service.BaseBiome().id &&
        resolved[0].base &&
        resolved[1].id ==
                forestId &&
        !resolved[1].base &&
        resolved[2].id ==
                duneId &&
        !resolved[2].base,
        "M30-13 biome blend must keep BaseBiome first and optional biomes in stable ID order independent of insertion/hash order.");

    Require(
        NearlyEqual(
            resolved[0].weight,
            0.125,
            1.0e-6) &&
        NearlyEqual(
            resolved[1].weight,
            0.625,
            1.0e-6) &&
        NearlyEqual(
            resolved[2].weight,
            0.25,
            1.0e-6),
        "M30-13 resolved biome weights must preserve eligible automatic/authored weights and assign the exact residual to BaseBiome.");

    f64 totalWeight = 0.0;

    for (const auto& weight :
         resolved)
    {
        totalWeight +=
            weight.weight;
    }

    Require(
        NearlyEqual(
            totalWeight,
            1.0,
            1.0e-6),
        "M30-13 resolved automatic/authored biome blend must close to unit coverage.");

    const auto replay =
        service.ResolvePlacement(
            context);

    Require(
        replay.size() ==
            resolved.size(),
        "M30-13 deterministic replay changed biome blend cardinality.");

    for (std::size_t index = 0U;
         index < resolved.size();
         ++index)
    {
        Require(
            replay[index].id ==
                    resolved[index].id &&
            replay[index].weight ==
                    resolved[index].weight &&
            replay[index].base ==
                    resolved[index].base,
            "M30-13 automatic/authored biome blend must replay bit-identically for fixed placement authority.");
    }

    // Force an oversubscribed case through the same production Resolve path.
    // Base remains present at zero and optionals normalize proportionally.
    const std::array<
        BiomeWeightContribution,
        2>
        oversubscribed{{
            {
                .id = forestId,
                .weight = 0.75F
            },
            {
                .id = duneId,
                .weight = 0.75F
            }
        }};

    const auto normalized =
        service.Resolve(
            oversubscribed);

    Require(
        normalized.size() ==
                3U &&
        normalized[0].base &&
        NearlyEqual(
            normalized[0].weight,
            0.0,
            1.0e-6) &&
        NearlyEqual(
            normalized[1].weight,
            0.5,
            1.0e-6) &&
        NearlyEqual(
            normalized[2].weight,
            0.5,
            1.0e-6),
        "M30-13 oversubscribed optional coverage must normalize proportionally while retaining exactly one zero-weight BaseBiome entry.");
}


void Test14ExposedRockMaterialResolution()
{
    using namespace surface_model;
    using namespace terrain_biome;
    using namespace terrain_material_column;

    StratigraphyFixture fixture;

    constexpr f32 regolithMeters = 0.50F;
    constexpr f32 soilMeters = 0.25F;
    constexpr f32 sandMeters = 0.125F;
    constexpr f32 debrisMeters = 0.125F;
    constexpr f64 totalCoverMeters = 1.0;

    MaterialColumnPage page(
        1U,
        2.0);

    page.SetCell(
        0U,
        0U,
        {
            .bedrockHeightMeters = 100.0F,
            .referenceBedrockHeightMeters = 100.0F,
            .bedrockMaterial =
                terrain_geology::
                    reference_rock::
                        Basalt,
            .regolithMeters =
                regolithMeters,
            .soilMeters =
                soilMeters,
            .sandMeters =
                sandMeters,
            .debrisMeters =
                debrisMeters,
            .moisture = 0.80F,
            .temporaryScalar = 0.0F
        });

    const universe::BodyId body{
        .high = 0x4D33305355524641ULL,
        .low = 0x4345000000000014ULL
    };

    const BiomeId biomeId{
        .high = 0x4D33305355524641ULL,
        .low = 0x4345000000001401ULL
    };

    BiomeDefinition biome{
        .id = biomeId,
        .name = "M30 exposed-rock dressing",
        .placement = {
            .minimumResolvedWeight = 0.0F,
            .enabled = true
        },
        .surface = {
            .materialInfluence = 1.0F,
            .layers = {
                BiomeSurfaceLayerRule{
                    .kind =
                        BiomeSurfaceLayerKind::
                            Moss,
                    .strength = 0.40F,
                    .compatibleExposed =
                        BiomeExposedMaterialMask::
                            Bedrock,
                    .minimumSlopeDegrees =
                        0.0F,
                    .maximumSlopeDegrees =
                        90.0F,
                    .slopeFalloffDegrees =
                        0.0F,
                    .minimumCurvature =
                        -1.0F,
                    .maximumCurvature =
                        1.0F,
                    .curvatureFalloff =
                        0.0F,
                    .minimumMoisture =
                        0.0F,
                    .maximumMoisture =
                        1.0F,
                    .moistureFalloff =
                        0.0F,
                    .enabled = true
                }
            }
        }
    };

    BiomeService biomes(
        body);

    biomes.UpsertBiome(
        biome);

    const std::array<
        BiomeWeightContribution,
        1>
        contribution{{
            {
                .id = biomeId,
                .weight = 1.0F
            }
        }};

    const auto biomeWeights =
        biomes.Resolve(
            contribution);

    const SurfaceMaterialFeatureMasks features{
        .slopeDegrees = 12.0F,
        .curvature = 0.0F,
        .snowCoverage = 0.0F,
        .mossPotential = 1.0F,
        .litterAvailability = 0.0F,
        .dustAvailability = 0.0F
    };

    const auto resolve =
        [&]()
        {
            const auto geology =
                SampleColumnGeology(
                    page.At(
                        0U,
                        0U),
                    fixture.materials);

            const auto physical =
                ResolveSurface(
                    page.At(
                        0U,
                        0U),
                    geology);

            const auto blend =
                ResolveSurfaceMaterialBlend(
                    physical,
                    biomes,
                    biomeWeights,
                    features);

            const auto render =
                terrain_render::
                    MakeSurfaceMaterialRenderInput(
                        blend);

            return
                std::tuple{
                    physical,
                    blend,
                    render
                };
        };

    const auto [
        coveredPhysical,
        coveredBlend,
        coveredRender] =
        resolve();

    Require(
        coveredPhysical.material ==
                ExposedSurfaceKind::
                    Debris &&
        coveredPhysical.substrateRock ==
                terrain_geology::
                    reference_rock::
                        Basalt &&
        !coveredPhysical.
            exposedRock.IsValid() &&
        NearlyEqual(
            coveredPhysical.
                exposedLayerDepthMeters,
            debrisMeters,
            0.0),
        "M30-14 initial M18 state must report the real topmost M08 debris while retaining buried basalt only as substrate identity.");

    Require(
        NearlyEqual(
            coveredBlend.Weight(
                RenderedSurfaceMaterialKind::
                    Debris),
            1.0,
            1.0e-6) &&
        NearlyEqual(
            coveredBlend.Weight(
                RenderedSurfaceMaterialKind::
                    Moss),
            0.0,
            0.0) &&
        !coveredRender.
            exposedBedrock.IsValid(),
        "M30-14 M21/renderer must not expose or dress buried basalt through incompatible loose cover.");

    const auto removed =
        page.Erode(
            0U,
            0U,
            totalCoverMeters,
            fixture.materials);

    Require(
        NearlyEqual(
            removed.debrisMeters,
            debrisMeters,
            0.0) &&
        NearlyEqual(
            removed.sandMeters,
            sandMeters,
            0.0) &&
        NearlyEqual(
            removed.soilMeters,
            soilMeters,
            0.0) &&
        NearlyEqual(
            removed.regolithMeters,
            regolithMeters,
            0.0) &&
        NearlyEqual(
            removed.bedrockMeters,
            0.0,
            0.0),
        "M30-14 cover stripping must reveal bedrock without shaving or replacing the authoritative M08 substrate.");

    const auto [
        exposedPhysical,
        exposedBlend,
        exposedRender] =
        resolve();

    Require(
        exposedPhysical.
            BedrockExposed() &&
        exposedPhysical.material ==
                ExposedSurfaceKind::
                    Bedrock &&
        exposedPhysical.substrateRock ==
                terrain_geology::
                    reference_rock::
                        Basalt &&
        exposedPhysical.exposedRock ==
                terrain_geology::
                    reference_rock::
                        Basalt &&
        exposedPhysical.geology.
            bedrockMaterial ==
                terrain_geology::
                    reference_rock::
                        Basalt,
        "M30-14 stripping the last loose layer must expose the exact M02 basalt identity already owned by M08.");

    const auto* basalt =
        fixture.materials.Find(
            terrain_geology::
                reference_rock::
                    Basalt);

    Require(
        basalt != nullptr &&
        exposedPhysical.geology.
                hardness ==
            basalt->hardness &&
        exposedPhysical.geology.
                cohesion ==
            basalt->cohesion &&
        exposedPhysical.geology.
                densityKgPerCubicMeter ==
            basalt->density,
        "M30-14 M18 exposed-rock geology must come from the matching M02 material record rather than renderer/biome constants.");

    Require(
        NearlyEqual(
            exposedBlend.Weight(
                RenderedSurfaceMaterialKind::
                    Bedrock),
            0.60,
            1.0e-6) &&
        NearlyEqual(
            exposedBlend.Weight(
                RenderedSurfaceMaterialKind::
                    Moss),
            0.40,
            1.0e-6) &&
        exposedRender.exposedBedrock ==
            terrain_geology::
                reference_rock::
                    Basalt &&
        NearlyEqual(
            exposedRender.TotalWeight(),
            1.0,
            2.0e-5),
        "M30-14 M21 may add a compatible moss overlay, but the renderer must retain exposed basalt identity underneath the deterministic normalized blend.");

    const auto substrateBeforeBurial =
        page.At(
            0U,
            0U).
            bedrockMaterial;

    const f64 deposited =
        page.Deposit(
            0U,
            0U,
            LooseMaterialKind::Sand,
            0.75);

    Require(
        NearlyEqual(
            deposited,
            0.75,
            1.0e-12) &&
        page.At(
            0U,
            0U).
            bedrockMaterial ==
                substrateBeforeBurial,
        "M30-14 sand deposition must cover the exposed rock without redefining geological substrate identity.");

    const auto [
        buriedPhysical,
        buriedBlend,
        buriedRender] =
        resolve();

    Require(
        buriedPhysical.material ==
                ExposedSurfaceKind::
                    Sand &&
        buriedPhysical.substrateRock ==
                terrain_geology::
                    reference_rock::
                        Basalt &&
        !buriedPhysical.
            exposedRock.IsValid() &&
        NearlyEqual(
            buriedPhysical.
                exposedLayerDepthMeters,
            0.75,
            1.0e-6),
        "M30-14 new M08 deposition must immediately replace bedrock as the canonical exposed surface while preserving buried basalt substrate.");

    Require(
        NearlyEqual(
            buriedBlend.Weight(
                RenderedSurfaceMaterialKind::
                    Sand),
            1.0,
            1.0e-6) &&
        NearlyEqual(
            buriedBlend.Weight(
                RenderedSurfaceMaterialKind::
                    Bedrock),
            0.0,
            0.0) &&
        NearlyEqual(
            buriedBlend.Weight(
                RenderedSurfaceMaterialKind::
                    Moss),
            0.0,
            0.0) &&
        !buriedRender.
            exposedBedrock.IsValid() &&
        NearlyEqual(
            buriedRender.TotalWeight(),
            1.0,
            2.0e-5),
        "M30-14 M21 and renderer must follow the newly exposed physical sand instead of caching the previously visible basalt/moss material.");
}

} // namespace

int main()
{
    Test01StratigraphyExposure();
    Test02BedrockStripping();
    Test03SedimentDepositionBurial();
    Test04HydraulicMassConservation();
    Test05AeolianMassConservation();
    Test06ThermalReposeConvergence();
    Test07CrossPageWaterFlux();
    Test08CrossPageSedimentFlux();
    Test09CrossPageDuneMigration();
    Test10FastFlowDrainageReference();
    Test11AuthoredCanyonPersistence();
    Test12BiomeFallback();
    Test13AutomaticAuthoredBiomeBlend();
    Test14ExposedRockMaterialResolution();

    std::cout
        << "Orbit V0.0.4 M30 validation: 14/20 deterministic cases passed.\n";
    return EXIT_SUCCESS;
}
