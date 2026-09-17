#include <orbit/terrain/TerrainPosition.hpp>
#include <orbit/terrain_impacts/ImpactField.hpp>
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
    std::cerr << "M07 failure: " << message << '\n';
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
        .radiusMeters = 1'737'400.0,
        .id = {
            .high = 0x4F524249544D3037ULL,
            .low = 0x0000000000000001ULL
        },
        .generationSeed = 0x0707070712345678ULL
    };
}

terrain_impacts::ImpactFieldId FieldId(const u64 low)
{
    return {
        .high = 0x4F524249544D3037ULL,
        .low = low
    };
}

terrain_impacts::ImpactId ImpactId(const u64 low)
{
    return {
        .high = 0x494D504143544D37ULL,
        .low = low
    };
}

terrain::PlanetSurfacePosition Position(
    const world::PlanetDefinition& planet,
    const math::Double3& direction)
{
    return {
        .planet = planet.id,
        .unitDirection = math::Normalize(direction),
        .radialOffsetMeters = 0.0
    };
}

terrain_impacts::ImpactRecord MakeImpact(
    const u64 id,
    const math::Double3& center,
    const f64 radius,
    const terrain_impacts::CraterProfileKind profile =
        terrain_impacts::CraterProfileKind::Simple,
    const f64 degradation = 0.0)
{
    return {
        .id = ImpactId(id),
        .centerUnitDirection = math::Normalize(center),
        .radiusMeters = radius,
        .profile = profile,
        .simpleDepthRatio = 0.18,
        .complexDepthRatio = 0.075,
        .rimHeightRatio = 0.035,
        .ejectaThicknessRatio = 0.012,
        .ejectaExtentRadii = 3.0,
        .rayStrength = 0.0,
        .rayCount = 0,
        .degradation = degradation,
        .ageOrder = id,
        .enabled = true,
        .authored = true
    };
}

terrain_impacts::ImpactFieldDefinition EmptyDefinition(
    const world::PlanetDefinition& planet,
    const u64 fieldLow)
{
    return {
        .id = FieldId(fieldLow),
        .planet = planet.id,
        .name = "M07 test impact field",
        .seed = 0x7172737475767778ULL,
        .procedural = {
            .count = 0,
            .minimumRadiusMeters = 1'000.0,
            .maximumRadiusMeters = 100'000.0,
            .cumulativeExponent = 2.0
        },
        .complexTransitionRadiusMeters = 18'000.0
    };
}

void TestMoonPresetIsDeterministicAndCraterDominated()
{
    const auto planet = MakePlanet();

    const auto preset =
        terrain_impacts::MakeMoonLikeImpactPreset(
            planet.id,
            FieldId(0x1000ULL),
            0x0102030405060708ULL);

    terrain_impacts::ImpactField a(
        planet, preset);
    terrain_impacts::ImpactField b(
        planet, preset);

    Require(
        a.ResolvedImpacts().size() == 768,
        "Moon-like preset must generate the configured crater population.");

    Require(
        a.ResolvedImpacts().size() ==
            b.ResolvedImpacts().size(),
        "Deterministic impact population size changed.");

    u32 small = 0;
    u32 large = 0;

    for (std::size_t index = 0;
         index < a.ResolvedImpacts().size();
         ++index)
    {
        const auto& ia = a.ResolvedImpacts()[index];
        const auto& ib = b.ResolvedImpacts()[index];

        Require(
            ia.id == ib.id &&
            ia.centerUnitDirection == ib.centerUnitDirection,
            "Procedural impact identity/placement must be deterministic.");

        RequireNear(
            ia.radiusMeters,
            ib.radiusMeters,
            0.0,
            "Procedural impact radius must be deterministic.");

        if (ia.radiusMeters < 12'000.0)
        {
            ++small;
        }
        if (ia.radiusMeters > 40'000.0)
        {
            ++large;
        }
    }

    Require(
        small > large * 3U,
        "Power-law crater size-frequency distribution must favor small craters.");

    const terrain::TerrainSampleFootprint footprint{
        .diameterMeters = 250.0
    };

    u32 craterCentersWithRelief = 0;
    for (std::size_t index = 0;
         index < 48;
         ++index)
    {
        const auto& impact =
            a.ResolvedImpacts()[index];

        const auto sample =
            a.Sample(
                Position(
                    planet,
                    impact.centerUnitDirection),
                footprint);

        if (sample.affectingImpacts > 0 &&
            (sample.excavationDepthMeters > 1.0 ||
             std::abs(sample.heightDeltaMeters) > 1.0))
        {
            ++craterCentersWithRelief;
        }
    }

    Require(
        craterCentersWithRelief >= 44,
        "Moon-like preset must produce crater-dominated geological relief without water/wind systems.");
}

void TestSimpleAndComplexProfiles()
{
    const auto planet = MakePlanet();
    const math::Double3 center =
        math::Normalize(math::Double3{0.7, 0.2, 0.68});

    auto simpleDef = EmptyDefinition(planet, 0x2000ULL);
    simpleDef.authoredImpacts.push_back(
        MakeImpact(
            0x2001ULL,
            center,
            60'000.0,
            terrain_impacts::CraterProfileKind::Simple));

    auto complexDef = EmptyDefinition(planet, 0x2100ULL);
    complexDef.authoredImpacts.push_back(
        MakeImpact(
            0x2101ULL,
            center,
            60'000.0,
            terrain_impacts::CraterProfileKind::Complex));

    terrain_impacts::ImpactField simple(
        planet, simpleDef);
    terrain_impacts::ImpactField complex(
        planet, complexDef);

    const terrain::TerrainSampleFootprint footprint{
        .diameterMeters = 100.0
    };

    const auto position =
        Position(planet, center);

    const auto s =
        simple.Sample(position, footprint);
    const auto c =
        complex.Sample(position, footprint);

    Require(
        s.heightDeltaMeters < 0.0 &&
        c.heightDeltaMeters < 0.0,
        "Both simple and complex crater centers must excavate terrain.");

    Require(
        c.heightDeltaMeters >
        s.heightDeltaMeters,
        "Complex crater central peak/shallower bowl must differ from simple profile.");
}

void TestOverlapModifiesPriorCrater()
{
    const auto planet = MakePlanet();
    const math::Double3 center =
        math::Normalize(math::Double3{0.35, 0.88, 0.32});

    auto singleDef = EmptyDefinition(planet, 0x3000ULL);
    singleDef.authoredImpacts.push_back(
        MakeImpact(
            0x3001ULL,
            center,
            32'000.0));

    auto overlapDef = singleDef;
    overlapDef.id = FieldId(0x3100ULL);
    overlapDef.authoredImpacts.push_back(
        MakeImpact(
            0x3002ULL,
            center,
            18'000.0));

    terrain_impacts::ImpactField single(
        planet, singleDef);
    terrain_impacts::ImpactField overlap(
        planet, overlapDef);

    const terrain::TerrainSampleFootprint footprint{
        .diameterMeters = 50.0
    };

    const auto position =
        Position(planet, center);

    const auto before =
        single.Sample(position, footprint);
    const auto after =
        overlap.Sample(position, footprint);

    Require(
        after.affectingImpacts >= 2,
        "Overlapping craters must both participate in the derived terrain.");

    Require(
        std::abs(
            after.heightDeltaMeters -
            before.heightDeltaMeters) >
        100.0,
        "A younger overlapping crater must modify terrain already shaped by an older crater.");
}

void TestDegradationCanSoftenAndFillCraterRelief()
{
    const auto planet = MakePlanet();
    const math::Double3 center =
        math::Normalize(math::Double3{-0.41, 0.53, 0.74});

    auto pristineDef = EmptyDefinition(planet, 0x4000ULL);
    pristineDef.authoredImpacts.push_back(
        MakeImpact(
            0x4001ULL,
            center,
            45'000.0,
            terrain_impacts::CraterProfileKind::Simple,
            0.0));

    auto degradedDef = EmptyDefinition(planet, 0x4100ULL);
    degradedDef.authoredImpacts.push_back(
        MakeImpact(
            0x4101ULL,
            center,
            45'000.0,
            terrain_impacts::CraterProfileKind::Simple,
            0.8));

    terrain_impacts::ImpactField pristine(
        planet, pristineDef);
    terrain_impacts::ImpactField degraded(
        planet, degradedDef);

    const terrain::TerrainSampleFootprint footprint{
        .diameterMeters = 100.0
    };

    const auto position =
        Position(planet, center);

    const auto p =
        pristine.Sample(position, footprint);
    const auto d =
        degraded.Sample(position, footprint);

    Require(
        std::abs(d.heightDeltaMeters) <
        std::abs(p.heightDeltaMeters) * 0.25,
        "Crater degradation must strongly reduce topographic relief.");

    Require(
        d.excavationDepthMeters <
        p.excavationDepthMeters,
        "Degradation must reduce preserved excavation depth so later erosion/deposition can erase craters.");
}

void TestEjectaRaysAndDebrisFields()
{
    const auto planet = MakePlanet();
    const math::Double3 centerDirection =
        math::Normalize(math::Double3{0.2, 0.95, -0.24});

    auto definition = EmptyDefinition(planet, 0x5000ULL);
    auto impact =
        MakeImpact(
            0x5001ULL,
            centerDirection,
            35'000.0);
    impact.rayStrength = 1.2;
    impact.rayCount = 6;
    impact.ejectaExtentRadii = 4.0;
    definition.authoredImpacts.push_back(impact);

    terrain_impacts::ImpactField field(
        planet, definition);

    const auto center =
        Position(planet, centerDirection);
    const world::SurfaceFrame frame =
        terrain::SurfaceTangentFrame(center);

    const terrain::TerrainSampleFootprint footprint{
        .diameterMeters = 50.0
    };

    f64 maxRay = 0.0;
    f64 maxEjecta = 0.0;
    f64 maxDebris = 0.0;

    for (u32 index = 0; index < 48; ++index)
    {
        const f64 angle =
            (2.0 * std::numbers::pi_v<f64> *
             static_cast<f64>(index)) /
            48.0;

        const f64 distance =
            impact.radiusMeters * 1.35;

        const auto samplePosition =
            terrain::OffsetSurfacePosition(
                planet,
                center,
                frame,
                {
                    std::cos(angle) * distance,
                    std::sin(angle) * distance
                });

        const auto sample =
            field.Sample(
                samplePosition,
                footprint);

        maxRay = std::max(
            maxRay, sample.rayField);
        maxEjecta = std::max(
            maxEjecta,
            sample.ejectaThicknessMeters);
        maxDebris = std::max(
            maxDebris,
            sample.debrisField);
    }

    Require(
        maxRay > 0.05,
        "Ray-enabled crater must expose a nonzero ray field.");
    Require(
        maxEjecta > 1.0,
        "Crater exterior must contain an ejecta blanket.");
    Require(
        maxDebris > 0.0,
        "Crater ejecta/rim must expose a debris field for M08+.");
}

void TestAuthoredImpactRoundTrip()
{
    const auto planet = MakePlanet();
    auto definition =
        EmptyDefinition(planet, 0x6000ULL);

    auto authored =
        MakeImpact(
            0x6001ULL,
            {0.4, 0.8, -0.44},
            22'000.0,
            terrain_impacts::CraterProfileKind::Complex,
            0.17);
    authored.rayStrength = 0.8;
    authored.rayCount = 5;
    authored.ageOrder = 1234567890123456789ULL;
    definition.authoredImpacts.push_back(authored);

    const std::string encoded =
        terrain_impacts::SerializeImpactFieldToml(
            definition);

    const auto decoded =
        terrain_impacts::ParseImpactFieldToml(
            encoded);

    Require(
        decoded.id == definition.id &&
        decoded.planet == definition.planet &&
        decoded.name == definition.name,
        "Authored impact field identity must survive TOML round-trip.");

    Require(
        decoded.authoredImpacts.size() == 1,
        "Authored crater placement must survive TOML round-trip.");

    const auto& crater =
        decoded.authoredImpacts.front();

    Require(
        crater.id == authored.id &&
        crater.profile == authored.profile &&
        crater.rayCount == authored.rayCount &&
        crater.ageOrder == authored.ageOrder,
        "Authored crater process metadata must persist exactly.");

    RequireNear(
        crater.radiusMeters,
        authored.radiusMeters,
        0.0,
        "Authored crater radius changed during persistence.");

    terrain_impacts::ImpactField original(
        planet, definition);
    terrain_impacts::ImpactField rebuilt(
        planet, decoded);

    const terrain::TerrainSampleFootprint footprint{
        .diameterMeters = 100.0
    };
    const auto position =
        Position(
            planet,
            authored.centerUnitDirection);

    RequireNear(
        original.Sample(position, footprint).
            heightDeltaMeters,
        rebuilt.Sample(position, footprint).
            heightDeltaMeters,
        1.0e-10,
        "Discarding derived terrain and rebuilding from authored impacts must reproduce crater relief.");
}
} // namespace

int main()
{
    TestMoonPresetIsDeterministicAndCraterDominated();
    TestSimpleAndComplexProfiles();
    TestOverlapModifiesPriorCrater();
    TestDegradationCanSoftenAndFillCraterRelief();
    TestEjectaRaysAndDebrisFields();
    TestAuthoredImpactRoundTrip();

    std::cout << "Orbit M07 impact/crater tests passed.\n";
    return EXIT_SUCCESS;
}
