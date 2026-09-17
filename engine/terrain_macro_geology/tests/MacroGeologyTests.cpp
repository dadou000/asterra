#include <orbit/surface_authoring/TerrainConstraints.hpp>
#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain/TerrainPosition.hpp>
#include <orbit/terrain_macro_geology/MacroGeologyField.hpp>
#include <orbit/world/Planet.hpp>

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
    std::cerr << "M05 failure: " << message << '\n';
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
    const f64 epsilon,
    const std::string& message)
{
    if (std::abs(a - b) > epsilon)
    {
        Fail(message + " (" + std::to_string(a) +
             " vs " + std::to_string(b) + ")");
    }
}

world::PlanetDefinition MakePlanet()
{
    return {
        .radiusMeters = 6'000'000.0,
        .id = {
            .high = 0x4F524249544D3035ULL,
            .low = 0x0000000000000001ULL
        },
        .generationSeed = 0x123456789ABCDEF0ULL
    };
}

surface_authoring::TerrainConstraintSet MakeAuthoredSet(
    const world::PlanetDefinition& planet,
    const f64 mountainUplift)
{
    surface_authoring::TerrainConstraintSet set{
        .id = {
            .high = 0x4F524249544D3035ULL,
            .low = 0x0000000000000100ULL
        },
        .planet = planet.id,
        .name = "M05 acceptance"
    };

    set.uplift.constraints.push_back(
        surface_authoring::ScalarTerrainConstraint{
            .id = {
                .high = 0x4F524249544D3035ULL,
                .low = 0x0000000000000101ULL
            },
            .mode =
                surface_authoring::ConstraintCompositionMode::Add,
            .primitive =
                surface_authoring::BrushConstraintPrimitive{
                    .centerUnitDirection =
                        math::Normalize(
                            math::Double3{1.0, 0.0, 1.0}),
                    .innerRadiusMeters = 250'000.0,
                    .outerRadiusMeters = 400'000.0
                },
            .value = mountainUplift,
            .opacity = 1.0,
            .enabled = true
        });

    set.drainage.constraints.push_back(
        surface_authoring::ScalarTerrainConstraint{
            .id = {
                .high = 0x4F524249544D3035ULL,
                .low = 0x0000000000000102ULL
            },
            .mode =
                surface_authoring::ConstraintCompositionMode::Replace,
            .primitive =
                surface_authoring::BrushConstraintPrimitive{
                    .centerUnitDirection =
                        math::Normalize(
                            math::Double3{1.0, 0.0, 1.0}),
                    .innerRadiusMeters = 300'000.0,
                    .outerRadiusMeters = 450'000.0
                },
            .value = 0.73,
            .opacity = 1.0,
            .enabled = true
        });

    return set;
}

void TestAuthoredUpliftChangesMountainForcingWithoutOverwritingDrainage()
{
    const world::PlanetDefinition planet = MakePlanet();

    terrain::GlobalTerrainFieldDesc globalDesc{};
    globalDesc.seed = 0xA55AA55AA55AA55AULL;
    terrain::GlobalTerrainFields globals(planet, globalDesc);

    const terrain::PlanetSurfacePosition position{
        .planet = planet.id,
        .unitDirection =
            math::Normalize(math::Double3{1.0, 0.0, 1.0}),
        .radialOffsetMeters = 0.0
    };

    auto lowSet = MakeAuthoredSet(planet, 900.0);
    auto highSet = MakeAuthoredSet(planet, 2'700.0);

    terrain_macro_geology::MacroGeologyField lowField(
        planet, globals, &lowSet);
    terrain_macro_geology::MacroGeologyField highField(
        planet, globals, &highSet);

    const auto low = lowField.Sample(position);
    const auto high = highField.Sample(position);

    Require(
        high.upliftMeters > low.upliftMeters + 1'700.0,
        "Increasing authored uplift must move macro mountain forcing.");

    RequireNear(
        low.drainageGuidance,
        high.drainageGuidance,
        1.0e-12,
        "Uplift edits must preserve independent M04 drainage authority.");

    RequireNear(
        low.drainageGuidance,
        0.73,
        1.0e-6,
        "Drainage guidance must survive macro-geology evaluation.");
}

void TestMountainBeltAndBasinUseM04Authority()
{
    const world::PlanetDefinition planet = MakePlanet();

    surface_authoring::SplineConstraintPrimitive belt{
        .controlUnitDirections = {
            math::Normalize(math::Double3{1.0, 0.0, 0.5}),
            math::Normalize(math::Double3{0.7, 0.0, 1.0})
        },
        .halfWidthMeters = 80'000.0,
        .falloffMeters = 40'000.0
    };

    const auto mountain =
        terrain_macro_geology::MakeMountainBeltConstraint(
            {
                .high = 0x4F524249544D3035ULL,
                .low = 0x0000000000000201ULL
            },
            belt,
            2'400.0);

    Require(
        mountain.value > 0.0,
        "Mountain-belt helper must author positive M04 uplift.");

    const auto basin =
        terrain_macro_geology::MakeBasinConstraint(
            {
                .high = 0x4F524249544D3035ULL,
                .low = 0x0000000000000202ULL
            },
            surface_authoring::BrushConstraintPrimitive{
                .centerUnitDirection = {0.0, 1.0, 0.0},
                .innerRadiusMeters = 100'000.0,
                .outerRadiusMeters = 180'000.0
            },
            1'100.0);

    Require(
        basin.value < 0.0,
        "Basin helper must author negative uplift/subsidence.");

    surface_authoring::TerrainConstraintSet set{
        .id = {
            .high = 0x4F524249544D3035ULL,
            .low = 0x0000000000000200ULL
        },
        .planet = planet.id,
        .name = "Belts and basins"
    };
    set.uplift.constraints.push_back(mountain);
    set.uplift.constraints.push_back(basin);

    Require(
        set.IsValid(),
        "M05 helpers must create ordinary valid M04 authority records.");
}

void TestImportedRasterCanDriveUplift()
{
    const world::PlanetDefinition planet = MakePlanet();

    terrain::GlobalTerrainFieldDesc globalDesc{};
    globalDesc.seed = 0x4142434445464748ULL;
    terrain::GlobalTerrainFields globals(planet, globalDesc);

    surface_authoring::TerrainConstraintSet set{
        .id = {
            .high = 0x4F524249544D3035ULL,
            .low = 0x0000000000000300ULL
        },
        .planet = planet.id,
        .name = "Raster uplift"
    };

    set.uplift.constraints.push_back({
        .id = {
            .high = 0x4F524249544D3035ULL,
            .low = 0x0000000000000301ULL
        },
        .mode =
            surface_authoring::ConstraintCompositionMode::Add,
        .primitive =
            surface_authoring::RasterMaskConstraintPrimitive{
                .anchorUnitDirection = {0.0, 1.0, 0.0},
                .rotationRadians = 0.0,
                .width = 3,
                .height = 3,
                .cellSizeMeters = 10'000.0,
                .samples = {
                    0.0F, 0.0F, 0.0F,
                    0.0F, 1.0F, 0.0F,
                    0.0F, 0.0F, 0.0F
                }
            },
        .value = 1'500.0,
        .opacity = 1.0,
        .enabled = true
    });

    terrain_macro_geology::MacroGeologyField authored(
        planet, globals, &set);
    terrain_macro_geology::MacroGeologyField baseline(
        planet, globals, nullptr);

    const terrain::PlanetSurfacePosition center{
        .planet = planet.id,
        .unitDirection = {0.0, 1.0, 0.0},
        .radialOffsetMeters = 0.0
    };

    const auto a = authored.Sample(center);
    const auto b = baseline.Sample(center);

    Require(
        a.upliftMeters > b.upliftMeters + 1'400.0,
        "Imported raster masks must be usable as M05 uplift sources.");
}

void TestDeterministicBodySpaceDistortion()
{
    const world::PlanetDefinition planet = MakePlanet();

    terrain::GlobalTerrainFieldDesc globalDesc{};
    globalDesc.seed = 0x1111222233334444ULL;
    terrain::GlobalTerrainFields globalsA(planet, globalDesc);
    terrain::GlobalTerrainFields globalsB(planet, globalDesc);

    terrain_macro_geology::MacroGeologyDesc desc{};
    desc.seed = 0x8877665544332211ULL;
    desc.distortionAmplitude = 0.35;
    desc.distortionWavelengthMeters = 900'000.0;
    desc.distortionOctaves = 4;

    terrain_macro_geology::MacroGeologyField a(
        planet, globalsA, nullptr, desc);
    terrain_macro_geology::MacroGeologyField b(
        planet, globalsB, nullptr, desc);

    const terrain::PlanetSurfacePosition position{
        .planet = planet.id,
        .unitDirection =
            math::Normalize(math::Double3{0.61, 0.22, -0.76}),
        .radialOffsetMeters = 0.0
    };

    const auto sa = a.Sample(position);
    const auto sb = b.Sample(position);

    RequireNear(
        sa.distortionSignal,
        sb.distortionSignal,
        0.0,
        "Body-space geological distortion must be deterministic.");

    RequireNear(
        sa.upliftMeters,
        sb.upliftMeters,
        0.0,
        "Same seed and physical position must produce identical macro uplift.");
}

void TestMacroRevisionExcludesFineDetailState()
{
    const terrain_macro_geology::MacroGeologyRevisionInputs inputs{
        .geology = 41,
        .authoring = 73
    };

    const u64 first =
        terrain_macro_geology::MacroGeologyRevisionFingerprint(inputs);
    const u64 second =
        terrain_macro_geology::MacroGeologyRevisionFingerprint({
            .geology = 41,
            .authoring = 73
        });

    Require(
        first == second,
        "Macro uplift revision must depend only on geology and authoring.");

    Require(
        first !=
            terrain_macro_geology::MacroGeologyRevisionFingerprint({
                .geology = 42,
                .authoring = 73
            }),
        "Geology revision must invalidate macro uplift.");

    Require(
        first !=
            terrain_macro_geology::MacroGeologyRevisionFingerprint({
                .geology = 41,
                .authoring = 74
            }),
        "Authoring revision must invalidate macro uplift.");
}

void TestCrossPlanetAuthorityRejected()
{
    const world::PlanetDefinition planet = MakePlanet();

    terrain::GlobalTerrainFieldDesc globalDesc{};
    terrain::GlobalTerrainFields globals(planet, globalDesc);

    auto set = MakeAuthoredSet(planet, 1'000.0);
    set.planet.low ^= 1ULL;

    bool rejected = false;
    try
    {
        terrain_macro_geology::MacroGeologyField field(
            planet, globals, &set);
        (void)field;
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }

    Require(
        rejected,
        "M05 must reject authored constraints from another planet.");
}
} // namespace

int main()
{
    TestAuthoredUpliftChangesMountainForcingWithoutOverwritingDrainage();
    TestMountainBeltAndBasinUseM04Authority();
    TestImportedRasterCanDriveUplift();
    TestDeterministicBodySpaceDistortion();
    TestMacroRevisionExcludesFineDetailState();
    TestCrossPlanetAuthorityRejected();

    std::cout << "Orbit M05 macro geology tests passed.\n";
    return EXIT_SUCCESS;
}
