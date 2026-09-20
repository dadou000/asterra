#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_geology/Stratigraphy.hpp>
#include <orbit/terrain_erosion/AeolianErosion.hpp>
#include <orbit/terrain_erosion/HydraulicErosion.hpp>
#include <orbit/terrain_erosion/SedimentExchange.hpp>
#include <orbit/terrain_erosion/ThermalErosion.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/terrain_region/SurfaceBoundaryExchange.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
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

    std::cout
        << "Orbit V0.0.4 M30 validation: 7/20 deterministic cases passed.\n";
    return EXIT_SUCCESS;
}
