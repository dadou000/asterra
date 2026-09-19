#include <orbit/terrain/TerrainPosition.hpp>
#include <orbit/terrain_erosion/MultiScaleTerrain.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M23 failure: " << message << '\n';
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
        Fail(message);
    }
}

void TestDefaultProfileIsAContiguousPhysicalHierarchy()
{
    const auto profile =
        terrain_erosion::DefaultMultiScaleTerrainProfile();

    Require(
        profile.IsValid(),
        "Default physical hierarchy must validate.");

    const auto& macro = profile.levels[0];
    const auto& regional = profile.levels[1];
    const auto& local = profile.levels[2];
    const auto& fine = profile.levels[3];
    const auto& micro = profile.levels[4];

    Require(
        macro.sampleSpacingMeters >
            regional.sampleSpacingMeters &&
        regional.sampleSpacingMeters >
            local.sampleSpacingMeters &&
        local.sampleSpacingMeters >
            fine.sampleSpacingMeters &&
        fine.sampleSpacingMeters >
            micro.sampleSpacingMeters,
        "Physical spacing must strictly refine from macro to micro.");

    RequireNear(
        regional.maximumFeatureWavelengthMeters,
        macro.minimumFeatureWavelengthMeters,
        0.0,
        "Regional and macro frequency shells must touch.");
    RequireNear(
        local.maximumFeatureWavelengthMeters,
        regional.minimumFeatureWavelengthMeters,
        0.0,
        "Local and regional frequency shells must touch.");
    RequireNear(
        fine.maximumFeatureWavelengthMeters,
        local.minimumFeatureWavelengthMeters,
        0.0,
        "Fine and local frequency shells must touch.");
    RequireNear(
        micro.maximumFeatureWavelengthMeters,
        fine.minimumFeatureWavelengthMeters,
        0.0,
        "Micro and fine frequency shells must touch.");

    for (const auto& level : profile.levels)
    {
        Require(
            level.sampleSpacingMeters * 2.0 <=
                level.minimumFeatureWavelengthMeters,
            "Each physical tier must satisfy its Nyquist support.");
    }
}

void TestPhysicalSelectionOnlyAddsFinerTiers()
{
    const terrain_erosion::MultiScaleTerrainPlanner planner;

    const auto macro = planner.Select(400.0);
    const auto regional = planner.Select(40.0);
    const auto local = planner.Select(4.0);
    const auto fine = planner.Select(0.4);
    const auto micro = planner.Select(0.05);

    Require(
        macro.activeLevelCount == 1 &&
        macro.finestScale ==
            terrain_erosion::PhysicalTerrainScale::Macro,
        "400 m physical sampling must select macro only.");

    Require(
        regional.activeLevelCount == 2 &&
        regional.Includes(
            terrain_erosion::PhysicalTerrainScale::Macro) &&
        regional.Includes(
            terrain_erosion::PhysicalTerrainScale::Regional),
        "Regional refinement must preserve macro and add regional.");

    Require(
        local.activeLevelCount == 3 &&
        local.Includes(
            terrain_erosion::PhysicalTerrainScale::Macro) &&
        local.Includes(
            terrain_erosion::PhysicalTerrainScale::Regional) &&
        local.Includes(
            terrain_erosion::PhysicalTerrainScale::Local),
        "Local refinement must be a strict superset of coarser tiers.");

    Require(
        fine.activeLevelCount == 4,
        "Fine physical refinement must add exactly one tier.");

    Require(
        micro.activeLevelCount ==
            static_cast<u8>(
                terrain_erosion::kPhysicalTerrainScaleCount),
        "Sub-micro request must activate all physical tiers.");

    Require(
        !macro.Allows(
            terrain_erosion::MultiScaleTerrainProcess::Hydraulic),
        "Macro terrain must not run local hydraulic work.");

    Require(
        local.Allows(
            terrain_erosion::MultiScaleTerrainProcess::Hydraulic) &&
        local.Allows(
            terrain_erosion::MultiScaleTerrainProcess::Aeolian),
        "Local tier must expose hydraulic and aeolian process families.");

    Require(
        micro.Allows(
            terrain_erosion::MultiScaleTerrainProcess::ProceduralDetail),
        "Micro tier must expose cheap procedural amplification.");
}

void TestFootprintSelectionIsPhysicalNotRenderLod()
{
    const terrain_erosion::MultiScaleTerrainPlanner planner;

    const terrain::TerrainSampleFootprint footprint{
        .diameterMeters = 4.0
    };

    const auto direct = planner.Select(4.0);
    const auto throughFootprint = planner.Select(footprint);

    Require(
        direct.activeLevelCount ==
            throughFootprint.activeLevelCount &&
        direct.finestScale ==
            throughFootprint.finestScale &&
        direct.activeProcessMask ==
            throughFootprint.activeProcessMask,
        "Selection must be determined solely by physical-meter support.");

    const auto localFootprint = planner.FootprintFor(
        terrain_erosion::PhysicalTerrainScale::Local);

    RequireNear(
        localFootprint.diameterMeters,
        planner.Level(
            terrain_erosion::PhysicalTerrainScale::Local).
                sampleSpacingMeters,
        0.0,
        "Tier footprint must use physical spacing directly.");
}

void TestFrequencyShellOwnershipIsStable()
{
    const terrain_erosion::MultiScaleTerrainPlanner planner;

    Require(
        planner.ScaleForFeatureWavelength(5'000.0) ==
            terrain_erosion::PhysicalTerrainScale::Macro,
        "Kilometer-scale features belong to macro.");
    Require(
        planner.ScaleForFeatureWavelength(500.0) ==
            terrain_erosion::PhysicalTerrainScale::Regional,
        "Hundreds-of-meters features belong to regional.");
    Require(
        planner.ScaleForFeatureWavelength(50.0) ==
            terrain_erosion::PhysicalTerrainScale::Local,
        "Tens-of-meters features belong to local.");
    Require(
        planner.ScaleForFeatureWavelength(5.0) ==
            terrain_erosion::PhysicalTerrainScale::Fine,
        "Meter-scale features belong to fine.");
    Require(
        planner.ScaleForFeatureWavelength(0.5) ==
            terrain_erosion::PhysicalTerrainScale::Micro,
        "Sub-meter features belong to micro.");

    Require(
        planner.ScaleForFeatureWavelength(0.05) ==
            terrain_erosion::PhysicalTerrainScale::Count,
        "Unsupported frequencies must not leak into another tier.");
}

void TestMacroIdentityDoesNotDependOnFineSelection()
{
    const terrain_erosion::MultiScaleTerrainPlanner planner;

    const auto coarseSelection = planner.Select(400.0);
    const auto fineSelection = planner.Select(0.4);

    Require(
        coarseSelection.Includes(
            terrain_erosion::PhysicalTerrainScale::Macro) &&
        fineSelection.Includes(
            terrain_erosion::PhysicalTerrainScale::Macro),
        "Both requests must retain the same macro tier.");

    constexpr u64 planetSeed = 0x123456789ABCDEF0ULL;
    constexpr u64 macroRevision = 17;

    const u64 macroKeyBefore = planner.StableScaleKey(
        terrain_erosion::PhysicalTerrainScale::Macro,
        planetSeed,
        macroRevision);
    const u64 macroKeyAfter = planner.StableScaleKey(
        terrain_erosion::PhysicalTerrainScale::Macro,
        planetSeed,
        macroRevision);

    Require(
        macroKeyBefore == macroKeyAfter,
        "Adding fine physical detail must not re-seed macro terrain.");

    Require(
        macroKeyBefore != planner.StableScaleKey(
            terrain_erosion::PhysicalTerrainScale::Local,
            planetSeed,
            macroRevision),
        "Different physical tiers need distinct stable identities.");

    Require(
        macroKeyBefore != planner.StableScaleKey(
            terrain_erosion::PhysicalTerrainScale::Macro,
            planetSeed,
            macroRevision + 1U),
        "A tier-owned revision must invalidate that tier identity.");
}

void TestTelemetryIsSeparatedByPhysicalTier()
{
    terrain_erosion::MultiScaleTerrainTelemetry telemetry;

    telemetry.Record(
        terrain_erosion::PhysicalTerrainScale::Macro,
        4096,
        0.4,
        1.2);
    telemetry.Record(
        terrain_erosion::PhysicalTerrainScale::Local,
        65536,
        0.2,
        3.5);

    const auto& macro = telemetry.Stats(
        terrain_erosion::PhysicalTerrainScale::Macro);
    const auto& local = telemetry.Stats(
        terrain_erosion::PhysicalTerrainScale::Local);

    Require(
        macro.executions == 1 &&
        macro.processedCells == 4096 &&
        local.executions == 1 &&
        local.processedCells == 65536,
        "Profiling must stay separated by physical tier.");

    RequireNear(
        macro.gpuMilliseconds,
        1.2,
        1.0e-12,
        "Macro GPU time must remain in the macro bucket.");
    RequireNear(
        local.gpuMilliseconds,
        3.5,
        1.0e-12,
        "Local GPU time must remain in the local bucket.");

    telemetry.Reset();
    Require(
        telemetry.Stats(
            terrain_erosion::PhysicalTerrainScale::Macro).
                executions == 0,
        "Telemetry reset must clear physical-tier counters.");
}

void TestInvalidProfilesAreRejected()
{
    auto invalid =
        terrain_erosion::DefaultMultiScaleTerrainProfile();

    invalid.levels[2].sampleSpacingMeters =
        invalid.levels[1].sampleSpacingMeters;

    bool threw = false;
    try
    {
        const terrain_erosion::MultiScaleTerrainPlanner planner(
            invalid);
        (void)planner;
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }

    Require(
        threw,
        "Non-refining physical profiles must be rejected.");
}
} // namespace

int main()
{
    TestDefaultProfileIsAContiguousPhysicalHierarchy();
    TestPhysicalSelectionOnlyAddsFinerTiers();
    TestFootprintSelectionIsPhysicalNotRenderLod();
    TestFrequencyShellOwnershipIsStable();
    TestMacroIdentityDoesNotDependOnFineSelection();
    TestTelemetryIsSeparatedByPhysicalTier();
    TestInvalidProfilesAreRejected();

    std::cout << "Orbit M23 multi-scale terrain tests passed.\n";
    return EXIT_SUCCESS;
}
