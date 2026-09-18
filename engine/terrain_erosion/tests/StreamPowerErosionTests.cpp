#include <orbit/terrain_erosion/StreamPowerErosion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace orbit;
using namespace orbit::terrain_erosion;
using namespace orbit::terrain_hydrology;
using namespace orbit::terrain_material_column;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M10 failure: " << message << '\n';
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

    library.Upsert({
        .id = terrain_geology::reference_rock::Basalt,
        .name = "M10 hard basalt",
        .hardness = 0.95F,
        .cohesion = 0.90F,
        .hydraulicErodibility = 0.12F,
        .aeolianErodibility = 0.04F,
        .permeability = 0.10F,
        .chemicalWeatherability = 0.20F,
        .fractureTendency = 0.30F,
        .density = 3'000.0F
    });

    library.Upsert({
        .id = terrain_geology::reference_rock::VolcanicAsh,
        .name = "M10 soft ash",
        .hardness = 0.10F,
        .cohesion = 0.10F,
        .hydraulicErodibility = 0.95F,
        .aeolianErodibility = 0.80F,
        .permeability = 0.60F,
        .chemicalWeatherability = 0.80F,
        .fractureTendency = 0.70F,
        .density = 1'700.0F
    });

    return library;
}

MaterialColumnCell Cell(
    const f32 height,
    const terrain_geology::RockTypeId rock,
    const f32 soilMeters = 0.0F)
{
    return {
        .bedrockHeightMeters = height,
        .referenceBedrockHeightMeters = height,
        .bedrockMaterial = rock,
        .regolithMeters = 0.0F,
        .soilMeters = soilMeters,
        .sandMeters = 0.0F,
        .debrisMeters = 0.0F,
        .moisture = 0.0F,
        .temporaryScalar = 0.0F
    };
}

terrain::PhysicalTerrainPageKey Key(
    const u32 resolution,
    const u64 processRevision = 1U)
{
    terrain::PhysicalTerrainPageKey key{};
    key.address.planet = {
        .high = 0x4D3130504C414E45ULL,
        .low = 0x5400000000000001ULL
    };
    key.address.tile = {
        .face = world::CubeFace::PositiveX,
        .level = 3U,
        .x = 3U,
        .y = 3U
    };
    key.resolution = resolution;
    key.revisions.geology = 2U;
    key.revisions.authoring = 3U;
    key.revisions.processes = processRevision;
    return key;
}

DrainageBoundaryCell Boundary(
    const f32 height)
{
    return {
        .surfaceHeightMeters = height,
        .conditionedHeightMeters = height,
        .authoredDrainage = 0.0F,
        .drainageAreaSquareMeters = 0.0,
        .dischargeCubicMetersPerSecond = 0.0,
        .flowDx = 0,
        .flowDy = 0
    };
}

DrainagePageHalo Halo(
    const u32 resolution,
    const f32 north,
    const f32 east,
    const f32 south,
    const f32 west,
    const u64 revision = 1U)
{
    DrainagePageHalo halo{};
    halo.revision = revision;
    halo.north.assign(resolution, Boundary(north));
    halo.east.assign(resolution, Boundary(east));
    halo.south.assign(resolution, Boundary(south));
    halo.west.assign(resolution, Boundary(west));

    const f32 high =
        std::max(
            std::max(north, south),
            std::max(east, west));

    halo.corners = {
        Boundary(high),
        Boundary(high),
        Boundary(high),
        Boundary(high)
    };

    return halo;
}

std::vector<DrainageCellInput> Inputs(
    const u32 resolution,
    const f32 runoff = 0.001F)
{
    return std::vector<DrainageCellInput>(
        static_cast<std::size_t>(resolution) *
            resolution,
        DrainageCellInput{
            .runoffMetersPerSecond = runoff,
            .authoredDrainage = 0.0F,
            .outlet = false
        });
}

std::vector<StreamPowerCellForcing> Forcing(
    const u32 resolution)
{
    return std::vector<StreamPowerCellForcing>(
        static_cast<std::size_t>(resolution) *
            resolution);
}

StreamPowerErosionConfig TestConfig()
{
    StreamPowerErosionConfig config{};
    config.iterations = 1U;
    config.upliftCouplingPerIteration = 0.0;
    config.authoredHeightRelaxation = 0.0;
    config.incisionCoefficientMetersPerIteration = 8.0;
    config.drainageExponent = 0.5;
    config.slopeExponent = 1.0;
    config.referenceDrainageAreaSquareMeters = 10'000.0;
    config.referenceDischargeCubicMetersPerSecond = 1.0;
    config.looseMaterialErodibility = 1.0;
    config.minimumBedSlope = 1.0e-5;
    config.maximumIncisionMetersPerIteration = 10.0;
    config.drainage.depressionPolicy =
        DepressionRoutingPolicy::FillToBoundary;
    config.drainage.minimumDrainageDropMeters = 0.001F;
    return config;
}

void TestDrainageAreaDrivesValleyIncisionAndBake()
{
    constexpr u32 resolution = 5U;
    constexpr f64 spacing = 100.0;

    auto geology = MakeGeology();
    MaterialColumnPage page(resolution, spacing);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            page.SetCell(
                x,
                y,
                Cell(
                    110.0F -
                        static_cast<f32>(x) * 2.0F,
                    terrain_geology::reference_rock::Basalt,
                    0.05F));
        }
    }

    auto inputs = Inputs(resolution);
    auto forcing = Forcing(resolution);
    auto halo = Halo(
        resolution,
        1'000.0F,
        98.0F,
        1'000.0F,
        1'000.0F);

    const auto result =
        SolveStreamPowerErosion(
            page,
            Key(resolution),
            geology,
            inputs,
            halo,
            forcing,
            TestConfig());

    const auto& upstream =
        result.At(0, 2);
    const auto& downstream =
        result.At(4, 2);

    Require(
        downstream.finalDrainageAreaSquareMeters >
            upstream.finalDrainageAreaSquareMeters,
        "Contributing drainage area must grow downstream.");

    Require(
        downstream.cumulativeIncisionMeters >
            upstream.cumulativeIncisionMeters,
        "Stream-power incision must strengthen as contributing area grows.");

    MaterialColumnPage baked = page;

    const auto bake =
        ApplyStreamPowerErosionResult(
            baked,
            geology,
            result);

    Require(
        bake.removedMassKg > 0.0 &&
        bake.totalIncisionMeters > 0.0,
        "M10 bake must report physical M08 material removal.");

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            RequireNear(
                baked.At(x, y).SurfaceHeightMeters(),
                result.At(x, y).finalSurfaceHeightMeters,
                2.0e-4,
                "Baked M08 surface does not match the solved M10 surface.");
        }
    }
}

void TestGeologyControlsIncision()
{
    constexpr u32 resolution = 4U;
    constexpr f64 spacing = 100.0;

    auto geology = MakeGeology();

    MaterialColumnPage hard(resolution, spacing);
    MaterialColumnPage soft(resolution, spacing);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            const f32 height =
                105.0F -
                static_cast<f32>(x);

            hard.SetCell(
                x,
                y,
                Cell(
                    height,
                    terrain_geology::reference_rock::Basalt));

            soft.SetCell(
                x,
                y,
                Cell(
                    height,
                    terrain_geology::reference_rock::VolcanicAsh));
        }
    }

    const auto inputs = Inputs(resolution);
    const auto forcing = Forcing(resolution);
    const auto halo = Halo(
        resolution,
        500.0F,
        100.0F,
        500.0F,
        500.0F);

    auto config = TestConfig();
    config.incisionCoefficientMetersPerIteration = 20.0;

    const auto hardResult =
        SolveStreamPowerErosion(
            hard,
            Key(resolution, 10U),
            geology,
            inputs,
            halo,
            forcing,
            config);

    const auto softResult =
        SolveStreamPowerErosion(
            soft,
            Key(resolution, 10U),
            geology,
            inputs,
            halo,
            forcing,
            config);

    Require(
        softResult.At(2, 1).lastErodibility >
            hardResult.At(2, 1).lastErodibility,
        "M02 intrinsic geology did not produce a stronger soft-rock erosion mask.");

    Require(
        softResult.At(2, 1).cumulativeIncisionMeters >
            hardResult.At(2, 1).cumulativeIncisionMeters,
        "Soft geological material must incise faster than competent bedrock.");
}

void TestProtectionAndAuthoredEquilibrium()
{
    constexpr u32 resolution = 3U;
    constexpr f64 spacing = 100.0;

    auto geology = MakeGeology();
    MaterialColumnPage page(resolution, spacing);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            page.SetCell(
                x,
                y,
                Cell(
                    104.0F -
                        static_cast<f32>(x),
                    terrain_geology::reference_rock::VolcanicAsh));
        }
    }

    const auto inputs = Inputs(resolution);
    auto forcing = Forcing(resolution);

    forcing[1U * resolution + 1U].protection = 1.0;

    auto config = TestConfig();
    config.incisionCoefficientMetersPerIteration = 20.0;

    const auto protectedResult =
        SolveStreamPowerErosion(
            page,
            Key(resolution, 20U),
            geology,
            inputs,
            Halo(
                resolution,
                500.0F,
                99.0F,
                500.0F,
                500.0F),
            forcing,
            config);

    RequireNear(
        protectedResult.At(1, 1).cumulativeIncisionMeters,
        0.0,
        1.0e-9,
        "Full M04 protection must suppress M10 incision.");

    // Disable incision and exercise geological uplift plus persistent authored
    // height relaxation independently.
    forcing = Forcing(resolution);
    forcing[1U * resolution + 1U].upliftForcingMeters = 100.0;
    forcing[1U * resolution + 1U].authoredElevationOffsetMeters = 10.0;

    config.iterations = 4U;
    config.incisionCoefficientMetersPerIteration = 0.0;
    config.upliftCouplingPerIteration = 0.01;
    config.authoredHeightRelaxation = 0.5;

    const auto uplifted =
        SolveStreamPowerErosion(
            page,
            Key(resolution, 21U),
            geology,
            inputs,
            Halo(
                resolution,
                500.0F,
                99.0F,
                500.0F,
                500.0F),
            forcing,
            config);

    const auto& center =
        uplifted.At(1, 1);

    Require(
        center.cumulativeBedrockDisplacementMeters > 0.0F &&
        center.finalSurfaceHeightMeters >
            center.initialSurfaceHeightMeters,
        "M05 uplift forcing must raise the M10 equilibrium surface.");

    // Four 50% relaxations toward +10 m plus +1 m tectonic displacement per
    // iteration should remain bounded around the authored target rather than
    // stamping +10 m every iteration.
    Require(
        center.finalSurfaceHeightMeters <
            center.initialSurfaceHeightMeters + 15.0F,
        "M04 height intent was re-applied as a repeated stamp instead of an equilibrium target.");

    MaterialColumnPage baked = page;
    static_cast<void>(
        ApplyStreamPowerErosionResult(
            baked,
            geology,
            uplifted));

    RequireNear(
        baked.QueryMass(geology).excavatedBedrockKg,
        0.0,
        1.0e-6,
        "M05 uplift/authored displacement must not be miscounted as bedrock excavation.");
}

void TestCrossPageStreamPowerBoundary()
{
    constexpr u32 resolution = 4U;
    constexpr f64 spacing = 100.0;

    auto geology = MakeGeology();

    MaterialColumnPage left(resolution, spacing);
    MaterialColumnPage right(resolution, spacing);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            left.SetCell(
                x,
                y,
                Cell(
                    108.0F -
                        static_cast<f32>(x),
                    terrain_geology::reference_rock::VolcanicAsh));

            right.SetCell(
                x,
                y,
                Cell(
                    104.0F -
                        static_cast<f32>(x),
                    terrain_geology::reference_rock::VolcanicAsh));
        }
    }

    const auto inputs = Inputs(resolution);
    const auto forcing = Forcing(resolution);

    DrainagePageHalo leftHalo =
        Halo(
            resolution,
            500.0F,
            104.0F,
            500.0F,
            500.0F,
            90U);

    DrainageRoutingConfig routing{};
    routing.minimumDrainageDropMeters = 0.001F;

    const auto leftDrainage =
        BuildDrainagePage(
            left,
            Key(resolution, 30U),
            inputs,
            leftHalo,
            routing);

    DrainagePageHalo rightHalo =
        Halo(
            resolution,
            500.0F,
            100.0F,
            500.0F,
            105.0F,
            91U);

    for (u32 y = 0; y < resolution; ++y)
    {
        rightHalo.west[y] =
            leftDrainage.BoundaryCell(
                DrainageBoundarySide::East,
                y);

        // Same-face neighbor orientation: imported west-halo flow enters east.
        rightHalo.west[y].flowDx = 1;
        rightHalo.west[y].flowDy = 0;
    }

    auto config = TestConfig();
    config.incisionCoefficientMetersPerIteration = 2.0;

    const auto leftResult =
        SolveStreamPowerErosion(
            left,
            Key(resolution, 30U),
            geology,
            inputs,
            leftHalo,
            forcing,
            config);

    const auto rightResult =
        SolveStreamPowerErosion(
            right,
            Key(resolution, 30U),
            geology,
            inputs,
            rightHalo,
            forcing,
            config);

    const f64 cellArea =
        spacing * spacing;

    RequireNear(
        leftResult.At(resolution - 1U, 1U).
            finalDrainageAreaSquareMeters,
        cellArea *
            static_cast<f64>(resolution),
        1.0e-6,
        "Left M10 page did not preserve drainage accumulation to the seam.");

    RequireNear(
        rightResult.At(0, 1).
            finalDrainageAreaSquareMeters,
        cellArea *
            static_cast<f64>(resolution + 1U),
        1.0e-6,
        "Right M10 page did not consume cross-page drainage area.");

    RequireNear(
        leftResult.At(resolution - 1U, 1U).lastSlope,
        rightResult.At(0, 1).lastSlope,
        2.0e-4,
        "M10 introduced a stream-power slope discontinuity at a physical page seam.");

    Require(
        rightResult.At(0, 1).cumulativeIncisionMeters >=
            leftResult.At(resolution - 1U, 1U).cumulativeIncisionMeters,
        "Cross-page contributing area should continue strengthening incision downstream.");
}

void TestDeterministicRevision()
{
    constexpr u32 resolution = 3U;

    auto geology = MakeGeology();
    MaterialColumnPage page(resolution, 100.0);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            page.SetCell(
                x,
                y,
                Cell(
                    103.0F -
                        static_cast<f32>(x),
                    terrain_geology::reference_rock::Basalt));
        }
    }

    auto forcing = Forcing(resolution);
    forcing[0].upliftForcingMeters = 20.0;

    const auto inputs = Inputs(resolution);
    const auto halo = Halo(
        resolution,
        500.0F,
        99.0F,
        500.0F,
        500.0F,
        123U);

    const auto config = TestConfig();

    const auto a =
        SolveStreamPowerErosion(
            page,
            Key(resolution, 40U),
            geology,
            inputs,
            halo,
            forcing,
            config);

    const auto b =
        SolveStreamPowerErosion(
            page,
            Key(resolution, 40U),
            geology,
            inputs,
            halo,
            forcing,
            config);

    Require(
        a.revision == b.revision,
        "Identical M10 inputs must have identical revision identity.");

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            const auto& lhs = a.At(x, y);
            const auto& rhs = b.At(x, y);

            Require(
                lhs.finalSurfaceHeightMeters ==
                    rhs.finalSurfaceHeightMeters &&
                lhs.cumulativeIncisionMeters ==
                    rhs.cumulativeIncisionMeters &&
                lhs.cumulativeBedrockDisplacementMeters ==
                    rhs.cumulativeBedrockDisplacementMeters,
                "M10 equilibrium solve is not deterministic.");
        }
    }
}
} // namespace

int main()
{
    TestDrainageAreaDrivesValleyIncisionAndBake();
    TestGeologyControlsIncision();
    TestProtectionAndAuthoredEquilibrium();
    TestCrossPageStreamPowerBoundary();
    TestDeterministicRevision();

    std::cout << "Orbit M10 stream-power erosion tests passed.\n";
    return EXIT_SUCCESS;
}
