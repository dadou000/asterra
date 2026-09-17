#include <orbit/surface_authoring/TerrainConstraints.hpp>
#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain/TerrainPosition.hpp>
#include <orbit/terrain_macro_geology/MacroGeologyField.hpp>
#include <orbit/terrain_relief/BaseReliefField.hpp>
#include <orbit/world/Planet.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M06 failure: " << message << '\n';
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

world::PlanetDefinition MakePlanet()
{
    return {
        .radiusMeters = 6'000'000.0,
        .id = {
            .high = 0x4F524249544D3036ULL,
            .low = 0x0000000000000001ULL
        },
        .generationSeed = 0x123456789ABCDEF0ULL
    };
}

terrain::PlanetSurfacePosition TestPosition(
    const world::PlanetDefinition& planet)
{
    return {
        .planet = planet.id,
        .unitDirection =
            math::Normalize(
                math::Double3{0.57, 0.41, -0.71}),
        .radialOffsetMeters = 0.0
    };
}

void TestAnalyticDerivativeMatchesFiniteDifference()
{
    const auto planet = MakePlanet();

    terrain_relief::BaseReliefDesc desc{};
    desc.seed = 0xABCDEF1122334455ULL;
    desc.continentalAmplitudeMeters = 2'500.0;
    desc.continentalWavelengthMeters = 2'400'000.0;
    desc.ridgeAmplitudeMeters = 700.0;
    desc.ridgeWavelengthMeters = 280'000.0;
    desc.ridgeOctaves = 4;
    desc.valleyAmplitudeMeters = 420.0;
    desc.valleyWavelengthMeters = 230'000.0;
    desc.valleyOctaves = 3;

    terrain_relief::BaseReliefField field(
        planet, nullptr, desc);

    const auto center = TestPosition(planet);
    const world::SurfaceFrame frame =
        terrain::SurfaceTangentFrame(center);
    const terrain::TerrainSampleFootprint footprint{
        .diameterMeters = 50.0
    };

    const auto analytic =
        field.Sample(center, footprint);

    constexpr f64 deltaMeters = 0.5;

    const auto eastPlus =
        terrain::OffsetSurfacePosition(
            planet, center, frame,
            {deltaMeters, 0.0});
    const auto eastMinus =
        terrain::OffsetSurfacePosition(
            planet, center, frame,
            {-deltaMeters, 0.0});
    const auto northPlus =
        terrain::OffsetSurfacePosition(
            planet, center, frame,
            {0.0, deltaMeters});
    const auto northMinus =
        terrain::OffsetSurfacePosition(
            planet, center, frame,
            {0.0, -deltaMeters});

    const f64 finiteEast =
        (field.Sample(eastPlus, footprint).heightMeters -
         field.Sample(eastMinus, footprint).heightMeters) /
        (2.0 * deltaMeters);

    const f64 finiteNorth =
        (field.Sample(northPlus, footprint).heightMeters -
         field.Sample(northMinus, footprint).heightMeters) /
        (2.0 * deltaMeters);

    RequireNear(
        analytic.derivative.east,
        finiteEast,
        2.5e-4,
        "Analytic east derivative must match finite difference.");

    RequireNear(
        analytic.derivative.north,
        finiteNorth,
        2.5e-4,
        "Analytic north derivative must match finite difference.");
}

void TestLodCutoffChangesAmplitudeNotPhase()
{
    const auto planet = MakePlanet();

    terrain_relief::BaseReliefDesc twoBand{};
    twoBand.seed = 0x77112233445566AAULL;
    twoBand.continentalAmplitudeMeters = 0.0;
    twoBand.continentalBiasMeters = 0.0;
    twoBand.ridgeAmplitudeMeters = 600.0;
    twoBand.ridgeWavelengthMeters = 320'000.0;
    twoBand.ridgeOctaves = 2;
    twoBand.valleyAmplitudeMeters = 0.0;
    twoBand.valleyOctaves = 0;
    twoBand.lacunarity = 4.0;
    twoBand.persistence = 0.5;

    auto oneBand = twoBand;
    oneBand.ridgeOctaves = 1;

    terrain_relief::BaseReliefField full(
        planet, nullptr, twoBand);
    terrain_relief::BaseReliefField reference(
        planet, nullptr, oneBand);

    const auto position = TestPosition(planet);

    // 80 km second octave is exactly removed by a 40 km footprint,
    // while the 320 km first octave remains fully represented.
    const terrain::TerrainSampleFootprint coarse{
        .diameterMeters = 40'000.0
    };

    const auto coarseFull =
        full.Sample(position, coarse);
    const auto coarseReference =
        reference.Sample(position, coarse);

    RequireNear(
        coarseFull.heightMeters,
        coarseReference.heightMeters,
        1.0e-10,
        "Removing an unresolvable octave must not rephase remaining bands.");

    RequireNear(
        coarseFull.derivative.east,
        coarseReference.derivative.east,
        1.0e-12,
        "LOD cutoff must preserve derivative phase of retained bands.");

    RequireNear(
        coarseFull.derivative.north,
        coarseReference.derivative.north,
        1.0e-12,
        "LOD cutoff must preserve derivative phase of retained bands.");
}

void TestFrequenciesFadeProgressively()
{
    constexpr f64 wavelength = 100'000.0;

    const f64 fullyRepresented =
        terrain_relief::ReliefFrequencyWeight(
            wavelength, 20'000.0);
    const f64 transition =
        terrain_relief::ReliefFrequencyWeight(
            wavelength, 30'000.0);
    const f64 removed =
        terrain_relief::ReliefFrequencyWeight(
            wavelength, 50'000.0);

    RequireNear(
        fullyRepresented,
        1.0,
        0.0,
        "Long-enough frequencies must be fully represented.");

    Require(
        transition > 0.0 && transition < 1.0,
        "Frequency cutoff must crossfade through a partial-weight range.");

    RequireNear(
        removed,
        0.0,
        0.0,
        "Frequencies at or below twice the footprint must be removed.");

    Require(
        fullyRepresented > transition &&
        transition > removed,
        "Frequency removal must be progressive and monotonic.");
}

void TestCanonicalPositionHasNoFacePhase()
{
    const auto planet = MakePlanet();

    terrain_relief::BaseReliefDesc desc{};
    desc.seed = 0x9999888877776666ULL;

    terrain_relief::BaseReliefField field(
        planet, nullptr, desc);

    const math::Double3 direction =
        math::Normalize(
            math::Double3{1.0, 0.13, 1.0});

    const terrain::PlanetSurfacePosition canonical{
        .planet = planet.id,
        .unitDirection = direction,
        .radialOffsetMeters = 0.0
    };

    const auto positiveX =
        terrain::SurfacePositionFromCube(
            planet.id,
            world::CubeCoordinate{
                .face = world::CubeFace::PositiveX,
                .uv = {direction.z / direction.x,
                       direction.y / direction.x}
            });

    const auto positiveZ =
        terrain::SurfacePositionFromCube(
            planet.id,
            world::CubeCoordinate{
                .face = world::CubeFace::PositiveZ,
                .uv = {direction.x / direction.z,
                       direction.y / direction.z}
            });

    const terrain::TerrainSampleFootprint footprint{
        .diameterMeters = 100.0
    };

    const auto a = field.Sample(canonical, footprint);
    const auto b = field.Sample(positiveX, footprint);
    const auto c = field.Sample(positiveZ, footprint);

    RequireNear(
        a.heightMeters, b.heightMeters, 1.0e-9,
        "Cube-face addressing must not change M06 relief phase.");
    RequireNear(
        a.heightMeters, c.heightMeters, 1.0e-9,
        "Adjacent cube-face addressing must share M06 phase.");
}

void TestM05ForcingIsForwardedButNotBakedIntoHeight()
{
    const auto planet = MakePlanet();

    terrain::GlobalTerrainFieldDesc globalDesc{};
    globalDesc.seed = 0x0102030405060708ULL;
    terrain::GlobalTerrainFields globals(
        planet, globalDesc);

    surface_authoring::TerrainConstraintSet authored{
        .id = {
            .high = 0x4F524249544D3036ULL,
            .low = 0x0000000000001000ULL
        },
        .planet = planet.id,
        .name = "M06 macro forwarding"
    };

    authored.uplift.constraints.push_back({
        .id = {
            .high = 0x4F524249544D3036ULL,
            .low = 0x0000000000001001ULL
        },
        .mode =
            surface_authoring::ConstraintCompositionMode::Add,
        .primitive =
            surface_authoring::BrushConstraintPrimitive{
                .centerUnitDirection =
                    TestPosition(planet).unitDirection,
                .innerRadiusMeters = 100'000.0,
                .outerRadiusMeters = 150'000.0
            },
        .value = 2'000.0,
        .opacity = 1.0,
        .enabled = true
    });

    terrain_macro_geology::MacroGeologyField macro(
        planet, globals, &authored);

    terrain_relief::BaseReliefDesc desc{};
    desc.seed = 0x5555666677778888ULL;

    terrain_relief::BaseReliefField withoutMacro(
        planet, nullptr, desc);
    terrain_relief::BaseReliefField withMacro(
        planet, &macro, desc);

    const auto position = TestPosition(planet);
    const terrain::TerrainSampleFootprint footprint{
        .diameterMeters = 100.0
    };

    const auto a = withoutMacro.Sample(
        position, footprint);
    const auto b = withMacro.Sample(
        position, footprint);
    const auto m = macro.Sample(position);

    RequireNear(
        a.heightMeters,
        b.heightMeters,
        0.0,
        "M05 uplift forcing must not be baked directly into M06 final height.");

    RequireNear(
        b.macroUpliftMeters,
        m.upliftMeters,
        0.0,
        "M06 must forward the exact M05 uplift forcing for later equilibrium.");
}
} // namespace

int main()
{
    TestAnalyticDerivativeMatchesFiniteDifference();
    TestLodCutoffChangesAmplitudeNotPhase();
    TestFrequenciesFadeProgressively();
    TestCanonicalPositionHasNoFacePhase();
    TestM05ForcingIsForwardedButNotBakedIntoHeight();

    std::cout << "Orbit M06 relief tests passed.\n";
    return EXIT_SUCCESS;
}
