#include <orbit/terrain_impacts/ImpactField.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

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

math::Double3 Position(
    const world::PlanetDefinition& planet,
    const math::Double3& direction)
{
    static_cast<void>(planet);
    return math::Normalize(direction);
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

    constexpr f64 footprint = 250.0;

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

void TestSpatialIndexAndReusableScratch()
{
    const auto planet = MakePlanet();
    auto definition = EmptyDefinition(planet, 0x2200ULL);
    definition.procedural = {
        .count = 12'000,
        .minimumRadiusMeters = 1'000.0,
        .maximumRadiusMeters = 20'000.0,
        .cumulativeExponent = 1.8
    };
    terrain_impacts::ImpactField field(planet, std::move(definition));
    const math::Double3 direction = math::Normalize(math::Double3{0.3, 0.8, -0.5});
    const std::size_t candidates = field.CandidateCount(direction);
    Require(candidates < field.ResolvedImpacts().size() / 8U,
        "Spherical hierarchy should prune most impacts at one sample location.");

    constexpr f64 footprint = 500.0;
    const auto position = Position(planet, direction);
    terrain_impacts::ImpactQueryScratch scratch;
    const auto first = field.Sample(position, footprint, scratch);
    const std::size_t retainedCapacity = scratch.candidates.capacity();
    const auto second = field.Sample(position, footprint, scratch);
    RequireNear(first.heightDeltaMeters, second.heightDeltaMeters, 0.0,
        "Reused query storage must preserve deterministic sampling.");
    Require(scratch.candidates.capacity() == retainedCapacity,
        "Repeated samples should reuse candidate storage after warm-up.");
    Require(scratch.traversalOverflow.empty(),
        "Traversal spill storage must be empty after a query completes.");
}

void TestTenMillionPopulationUsesStatisticalMicrocraters()
{
    const auto planet = MakePlanet();
    auto definition = EmptyDefinition(planet, 0x2300ULL);
    definition.procedural = {
        .count = 10'000'000U,
        .minimumRadiusMeters = 100.0,
        .maximumRadiusMeters = 100'000.0,
        .cumulativeExponent = 2.0
    };
    terrain_impacts::ImpactField field(planet, std::move(definition));
    Require(field.ResolvedImpacts().size() == 100'000U,
        "The explicit event tier should stay bounded at large population counts.");
    Require(field.StatisticalMicroImpactCount() == 9'900'000U,
        "Sub-resolution crater population should be represented statistically.");
    const auto sample = field.Sample(
        math::Normalize(math::Double3{0.2, 0.7, 0.6}), 1'000.0);
    Require(sample.microImpactCoverage > 0.0 &&
            sample.microImpactRoughnessMeters > 0.0 &&
            sample.microImpactRoughnessMeters <= 120.0,
        "Statistical microcrater coverage should contribute a footprint-bounded roughness channel.");
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

    constexpr f64 footprint = 100.0;

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
    auto youngOnlyDef = EmptyDefinition(planet, 0x3200ULL);
    youngOnlyDef.authoredImpacts.push_back(
        MakeImpact(0x3002ULL, center, 18'000.0));
    terrain_impacts::ImpactField youngOnly(planet, youngOnlyDef);

    constexpr f64 footprint = 50.0;

    const auto position =
        Position(planet, center);

    const auto before =
        single.Sample(position, footprint);
    const auto after =
        overlap.Sample(position, footprint);
    const auto young =
        youngOnly.Sample(position, footprint);

    Require(
        after.affectingImpacts >= 2,
        "Overlapping craters must both participate in the derived terrain.");

    Require(
        std::abs(
            after.heightDeltaMeters -
            before.heightDeltaMeters) >
        100.0,
        "A younger overlapping crater must modify terrain already shaped by an older crater.");
    RequireNear(after.heightDeltaMeters, young.heightDeltaMeters, 1.0e-8,
        "A younger crater that excavates the sample completely must replace the older local relief.");
    Require(after.exposureAgeOrder == 0x3002ULL &&
            after.formationAgeOrder == 0x3002ULL,
        "Complete younger excavation must reset local formation and exposure age.");
}

void BenchmarkThousandOverlappingImpacts()
{
    const auto planet = MakePlanet();
    const math::Double3 center =
        math::Normalize(math::Double3{0.41, -0.32, 0.85});
    auto definition = EmptyDefinition(planet, 0x3A00ULL);
    definition.authoredImpacts.reserve(1'000U);
    for (u64 index = 0U; index < 1'000U; ++index)
    {
        auto impact = MakeImpact(0x3A01ULL + index, center,
            500.0 + static_cast<f64>(index % 400U));
        impact.ageOrder = index + 1U;
        definition.authoredImpacts.push_back(std::move(impact));
    }
    terrain_impacts::ImpactField field(planet, std::move(definition));
    terrain_impacts::ImpactQueryScratch scratch;
    const auto started = std::chrono::steady_clock::now();
    const auto sample = field.Sample(center, 25.0, scratch);
    const f64 elapsedMilliseconds = std::chrono::duration<f64, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    Require(sample.affectingImpacts == 1'000U,
        "The overlapping-event stress case must process all 1,000 impacts.");
    std::cout << "Orbit geology stress: 1,000 overlapping impacts, "
              << elapsedMilliseconds << " ms/sample, "
              << field.CandidateCount(center) << " indexed candidates.\n";
}

void TestImpactsAndResurfacingShareChronology()
{
    const auto planet = MakePlanet();
    const math::Double3 center{0.0, 1.0, 0.0};
    const auto frame = world::MakeSurfaceFrame(center);
    const auto makeFlow = [&](const u64 ageOrder)
    {
        return terrain_impacts::ResurfacingRecord{
            .id = ImpactId(0x500U + ageOrder),
            .kind = terrain_impacts::ResurfacingKind::LavaFlow,
            .centerlineUnitDirections = {
                world::DirectionAtSurfaceOffset(planet, frame, {-40'000.0, 0.0}),
                center,
                world::DirectionAtSurfaceOffset(planet, frame, {40'000.0, 0.0})},
            .widthMeters = 30'000.0,
            .thicknessMeters = 1'000.0,
            .formationAgeYears = 1'000'000.0,
            .ageOrder = ageOrder};
    };

    auto flowAfterImpact = EmptyDefinition(planet, 101U);
    auto oldImpact = MakeImpact(1U, center, 80'000.0);
    oldImpact.ageOrder = 1U;
    flowAfterImpact.authoredImpacts.push_back(oldImpact);
    flowAfterImpact.resurfacingEvents.push_back(makeFlow(2U));
    const terrain_impacts::ImpactField resurfaced(planet, flowAfterImpact);
    const auto resurfacedSample = resurfaced.Sample(center, 100.0);
    Require(resurfacedSample.exposureAgeOrder == 2U &&
            resurfacedSample.formationAgeOrder == 2U,
        "A younger connected flow must become the exposed formation after an older crater.");
    Require(resurfacedSample.excavationDepthMeters < 1.0 &&
            resurfacedSample.resurfacedMaterialFraction > 0.99 &&
            resurfacedSample.resurfacingThicknessMeters > 999.0,
        "A younger connected flow must bury older crater excavation and deposit its material.");

    auto impactAfterFlow = EmptyDefinition(planet, 102U);
    auto youngImpact = MakeImpact(2U, center, 80'000.0);
    youngImpact.ageOrder = 2U;
    impactAfterFlow.resurfacingEvents.push_back(makeFlow(1U));
    impactAfterFlow.authoredImpacts.push_back(youngImpact);
    const terrain_impacts::ImpactField excavated(planet, impactAfterFlow);
    const auto excavatedSample = excavated.Sample(center, 100.0);
    Require(excavatedSample.exposureAgeOrder == 2U &&
            excavatedSample.excavationDepthMeters > 10'000.0,
        "A younger impact must excavate an older flow in shared chronology.");
    Require(excavatedSample.resurfacedMaterialFraction < 0.01 &&
            excavatedSample.resurfacingThicknessMeters < 1.0,
        "A younger impact must remove the older flow's exposed material at the crater center.");

    auto reactivatedHistory = EmptyDefinition(planet, 103U);
    reactivatedHistory.authoredImpacts.push_back(oldImpact);
    auto faultBelt = makeFlow(3U);
    faultBelt.kind = terrain_impacts::ResurfacingKind::TectonicRenewal;
    reactivatedHistory.resurfacingEvents.push_back(faultBelt);
    const terrain_impacts::ImpactField reactivated(planet, reactivatedHistory);
    const auto reactivatedSample = reactivated.Sample(center, 100.0);
    Require(reactivatedSample.exposureAgeOrder == 3U &&
            reactivatedSample.formationAgeOrder == 1U,
        "A younger tectonic reactivation must update exposure age while preserving crater formation age.");
    Require(reactivatedSample.brecciaField > 0.25 &&
            reactivatedSample.excavationDepthMeters > 10'000.0 &&
            reactivatedSample.resurfacedMaterialFraction < 0.01,
        "Tectonic renewal must deform and fracture the old crater without treating it as a lava blanket.");
}

void TestTectonicRenewalDisplacesOlderImpacts()
{
    const auto planet = MakePlanet();
    const math::Double3 center = math::Normalize(math::Double3{1.0, 0.0, 0.0});
    const world::SurfaceFrame frame = world::MakeSurfaceFrame(center);
    const math::Double3 oldCraterCenter = world::DirectionAtSurfaceOffset(
        planet, frame, {1'000.0, 0.0});
    auto definition = EmptyDefinition(planet, 0x3B00ULL);
    definition.authoredImpacts.push_back(MakeImpact(0x3B01ULL, oldCraterCenter, 5'000.0));
    terrain_impacts::ResurfacingRecord renewal{
        .id = ImpactId(0x3B02ULL),
        .kind = terrain_impacts::ResurfacingKind::TectonicRenewal,
        .centerlineUnitDirections={
            world::DirectionAtSurfaceOffset(planet, frame, {0.0, -50'000.0}),
            world::DirectionAtSurfaceOffset(planet, frame, {0.0, 50'000.0})},
        .widthMeters = 10'000.0,
        .thicknessMeters = 120.0,
        .formationAgeYears = 1'000.0,
        .displacementUnitDirection = frame.east,
        .displacementMeters = 2'000.0,
        .ageOrder = 0x3B10ULL};
    definition.resurfacingEvents.push_back(renewal);

    auto editedHistory = definition;
    editedHistory.resurfacingEvents.front().displacementMeters += 100.0;
    Require(!terrain_impacts::ChangedAuthoredEventInfluenceCaps(
                planet, definition, editedHistory).has_value(),
        "Changing tectonic slip must request a full geology rebuild.");

    terrain_impacts::ImpactField field(planet, std::move(definition));
    const math::Double3 presentCenter = field.ResolvedImpacts().front().centerUnitDirection;
    const f64 movedMeters = planet.radiusMeters * std::acos(std::clamp(
        math::Dot(oldCraterCenter, presentCenter), -1.0, 1.0));
    Require(movedMeters > 500.0 && movedMeters < 1'500.0,
        "A later tectonic renewal must displace an older crater along its fault slip direction.");

    auto plateHistory = EmptyDefinition(planet, 0x3C00ULL);
    plateHistory.authoredImpacts.push_back(MakeImpact(
        0x3C01ULL, oldCraterCenter, 2'500.0));
    terrain_impacts::ResurfacingRecord plateMotion{
        .id = ImpactId(0x3C02ULL),
        .kind = terrain_impacts::ResurfacingKind::TectonicRenewal,
        .centerlineUnitDirections = {
            world::DirectionAtSurfaceOffset(planet, frame, {-20'000.0, -20'000.0}),
            world::DirectionAtSurfaceOffset(planet, frame, {20'000.0, -20'000.0}),
            world::DirectionAtSurfaceOffset(planet, frame, {20'000.0, 20'000.0}),
            world::DirectionAtSurfaceOffset(planet, frame, {-20'000.0, 20'000.0}),
            world::DirectionAtSurfaceOffset(planet, frame, {-20'000.0, -20'000.0})},
        .widthMeters = 2'000.0,
        .thicknessMeters = 0.0,
        .formationAgeYears = 0.0,
        .displacementUnitDirection = frame.east,
        .displacementMeters = 4'000.0,
        .regionalPlateMotion = true,
        .ageOrder = 0x3C10ULL};
    plateHistory.resurfacingEvents.push_back(plateMotion);
    terrain_impacts::ImpactField plateField(planet, std::move(plateHistory));
    const f64 plateShiftMeters = planet.radiusMeters * std::acos(std::clamp(
        math::Dot(oldCraterCenter,
            plateField.ResolvedImpacts().front().centerUnitDirection),
        -1.0, 1.0));
    Require(plateShiftMeters > 3'900.0 && plateShiftMeters < 4'100.0,
        "A closed spherical tectonic boundary must move older structures with the plate history.");
}

void TestChronologicalEventBatchReferences()
{
    const auto planet = MakePlanet();
    auto definition = EmptyDefinition(planet, 104U);
    auto older = MakeImpact(0x610U, {0.0, 1.0, 0.0}, 20'000.0);
    older.ageOrder = 3U;
    auto newer = MakeImpact(0x602U, {0.0, 1.0, 0.0}, 20'000.0);
    newer.ageOrder = 1U;
    auto remote = MakeImpact(0x611U, {0.0, -1.0, 0.0}, 10'000.0);
    remote.ageOrder = 5U;
    definition.authoredImpacts = {older, newer, remote};
    const auto frame = world::MakeSurfaceFrame({0.0, 1.0, 0.0});
    const auto flow = [&](const u64 id, const u64 age, const terrain_impacts::ResurfacingKind kind)
    {
        return terrain_impacts::ResurfacingRecord{
            .id = ImpactId(id),
            .kind = kind,
            .centerlineUnitDirections = {
                world::DirectionAtSurfaceOffset(planet, frame, {-5'000.0, 0.0}),
                world::DirectionAtSurfaceOffset(planet, frame, {5'000.0, 0.0})},
            .widthMeters = 2'000.0,
            .thicknessMeters = 10.0,
            .ageOrder = age};
    };
    definition.resurfacingEvents = {
        flow(0x603U, 2U, terrain_impacts::ResurfacingKind::LavaFlow),
        flow(0x604U, 1U, terrain_impacts::ResurfacingKind::IceRenewal),
        flow(0x605U, 4U, terrain_impacts::ResurfacingKind::TectonicRenewal)};
    auto remoteFault = flow(0x612U, 5U, terrain_impacts::ResurfacingKind::TectonicRenewal);
    const auto remoteFrame = world::MakeSurfaceFrame({0.0, -1.0, 0.0});
    remoteFault.centerlineUnitDirections = {
        world::DirectionAtSurfaceOffset(planet, remoteFrame, {-5'000.0, 0.0}),
        world::DirectionAtSurfaceOffset(planet, remoteFrame, {5'000.0, 0.0})};
    definition.resurfacingEvents.push_back(remoteFault);

    const terrain_impacts::ImpactField field(planet, definition);
    const auto events = field.ChronologicalEvents();
    Require(events.size() == 7U,
        "The batch view must expose every resolved impact and connected geological event.");
    const auto& impacts = field.ResolvedImpacts();
    const auto& prepared = field.PreparedImpactGeometries();
    const auto& resurfacing = field.Definition().resurfacingEvents;
    Require(prepared.size() == impacts.size(),
        "Prepared impact geometry must keep one entry aligned with each resolved event.");
    for (std::size_t index = 0U; index < impacts.size(); ++index)
    {
        Require(math::Length(prepared[index].frame.up - impacts[index].centerUnitDirection) < 1.0e-9 &&
                std::isfinite(prepared[index].phase) &&
                std::isfinite(prepared[index].ejectaMassBalanceScale),
            "Prepared impact geometry must be finite and aligned to the canonical event index.");
    }
    const u64 expectedIds[] = {0x602U, 0x604U, 0x603U, 0x610U, 0x605U, 0x611U, 0x612U};
    const u64 expectedAges[] = {1U, 1U, 2U, 3U, 4U, 5U, 5U};
    for (std::size_t index = 0U; index < events.size(); ++index)
    {
        const auto& event = events[index];
        Require(event.id.low == expectedIds[index] &&
                event.ageOrder == expectedAges[index],
            "The batch view must sort by age and stable event ID.");
        const auto& referencedId = event.kind == terrain_impacts::GeologicalEventKind::Impact
            ? impacts[event.index].id
            : resurfacing[event.index].id;
        Require(referencedId == event.id,
            "Batch references must address the canonical immutable event arrays.");
    }
    Require(events[0].kind == terrain_impacts::GeologicalEventKind::Impact &&
            events[1].kind == terrain_impacts::GeologicalEventKind::IceRenewal &&
            events[2].kind == terrain_impacts::GeologicalEventKind::LavaFlow &&
            events[4].kind == terrain_impacts::GeologicalEventKind::TectonicRenewal,
        "The batch view must preserve each resurfacing process type.");
    const auto nearbyEvents = field.EventsIntersectingCap({0.0, 1.0, 0.0}, 0.2);
    Require(nearbyEvents.size() == 5U && nearbyEvents.back().id.low == 0x605U,
        "A regional event batch must include local impacts and deposits but prune distant events.");
    terrain_impacts::ImpactQueryScratch sampleScratch;
    const auto regularSample = field.Sample({0.0, 1.0, 0.0}, 100.0);
    const auto batchedSample = field.Sample(
        {0.0, 1.0, 0.0}, 100.0, sampleScratch, nearbyEvents);
    RequireNear(batchedSample.heightDeltaMeters, regularSample.heightDeltaMeters, 1.0e-6,
        "A conservative regional event batch must preserve the canonical relief sample.");
    RequireNear(batchedSample.exposureAgeYears, regularSample.exposureAgeYears, 1.0e-6,
        "A regional event batch must preserve exposure chronology.");
    Require(batchedSample.exposureAgeOrder == regularSample.exposureAgeOrder &&
            batchedSample.formationAgeOrder == regularSample.formationAgeOrder,
        "A regional event batch must preserve formation and exposure order.");
    const auto remoteEvents = field.EventsIntersectingCap({0.0, -1.0, 0.0}, 0.1);
    Require(remoteEvents.size() == 2U && remoteEvents[0].id.low == 0x611U &&
            remoteEvents[1].id.low == 0x612U,
        "A regional event batch must query resurfacing and impact indexes consistently.");
    terrain_impacts::ImpactQueryScratch batchScratch;
    std::vector<terrain_impacts::GeologicalEventReference> reusableBatch;
    field.CollectEventsIntersectingCap(
        {0.0, 1.0, 0.0}, 0.2, batchScratch, reusableBatch);
    const std::size_t retainedCapacity = reusableBatch.capacity();
    field.CollectEventsIntersectingCap(
        {0.0, -1.0, 0.0}, 0.1, batchScratch, reusableBatch);
    Require(reusableBatch.size() == 2U && reusableBatch[0].id.low == 0x611U &&
            reusableBatch.capacity() >= retainedCapacity,
        "Regional compilers must be able to reuse candidate scratch and output capacity across tiles.");
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

    constexpr f64 footprint = 100.0;

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

void TestEnvironmentAgeAndObliqueMorphology()
{
    const auto planet = MakePlanet();
    const math::Double3 center = math::Normalize(math::Double3{0.2, 0.7, 0.6});
    auto airlessDef = EmptyDefinition(planet, 0x4200ULL);
    airlessDef.surfaceAgeYears = 4.6e9;
    airlessDef.environment = terrain_impacts::SurfaceEnvironment::Airless;
    auto aged = MakeImpact(0x4201ULL, center, 40'000.0);
    aged.formationAgeYears = 4.5e9;
    airlessDef.authoredImpacts.push_back(aged);

    auto wetDef = airlessDef;
    wetDef.id = FieldId(0x4300ULL);
    wetDef.environment = terrain_impacts::SurfaceEnvironment::Wet;
    terrain_impacts::ImpactField airless(planet, airlessDef);
    terrain_impacts::ImpactField wet(planet, wetDef);
    constexpr f64 footprint = 100.0;
    const auto centerSample = airless.Sample(Position(planet, center), footprint);
    const auto wetSample = wet.Sample(Position(planet, center), footprint);
    Require(std::abs(wetSample.heightDeltaMeters) < std::abs(centerSample.heightDeltaMeters),
        "Wet-body age relaxation should degrade an ancient crater faster than airless gardening.");
    RequireNear(centerSample.exposureAgeYears, 100'000'000.0, 1.0,
        "Surface exposure age should report time since the latest event.");

    auto obliqueDef = EmptyDefinition(planet, 0x4400ULL);
    auto oblique = MakeImpact(0x4401ULL, center, 40'000.0);
    oblique.impactAngleDegrees = 80.0;
    oblique.shapeIrregularity = 0.12;
    oblique.impactAzimuthRadians = 0.0;
    oblique.meltFraction = 0.25;
    oblique.brecciaFraction = 0.8;
    obliqueDef.authoredImpacts.push_back(oblique);
    terrain_impacts::ImpactField shaped(planet, obliqueDef);
    const auto frame = world::MakeSurfaceFrame(center);
    const auto along = world::DirectionAtSurfaceOffset(planet, frame, {52'000.0, 0.0});
    const auto across = world::DirectionAtSurfaceOffset(planet, frame, {0.0, 52'000.0});
    const auto alongSample = shaped.Sample(Position(planet, along), footprint);
    const auto acrossSample = shaped.Sample(Position(planet, across), footprint);
    Require(alongSample.heightDeltaMeters < acrossSample.heightDeltaMeters,
        "Oblique impact should elongate its excavation along the trajectory azimuth.");
    const auto centerShaped = shaped.Sample(Position(planet, center), footprint);
    Require(centerShaped.meltThicknessMeters > 0.0 && centerShaped.brecciaField > 0.0,
        "Impact melt and breccia channels should be derived from authored material fractions.");
}

void TestImpactScalingUsesGravityAndTargetStrength()
{
    const terrain_impacts::ImpactScalingInput baseline{
        .impactorDiameterMeters = 100.0,
        .impactVelocityMetersPerSecond = 18'000.0,
        .impactAngleDegrees = 45.0,
        .impactorDensityKgPerCubicMeter = 3'000.0,
        .targetDensityKgPerCubicMeter = 2'700.0,
        .surfaceGravityMetersPerSecondSquared = 1.62,
        .targetStrengthPascals = 1'000'000.0
    };
    const f64 normal = terrain_impacts::ScaleImpactCraterRadiusMeters(baseline);
    auto strongerTarget = baseline;
    strongerTarget.targetStrengthPascals = 100'000'000.0;
    auto higherGravity = baseline;
    higherGravity.surfaceGravityMetersPerSecondSquared = 9.81;
    Require(normal > 0.0 &&
            terrain_impacts::ScaleImpactCraterRadiusMeters(strongerTarget) < normal &&
            terrain_impacts::ScaleImpactCraterRadiusMeters(higherGravity) < normal,
        "Impact scaling must respond to target strength and gravity in the excavation regime.");
    auto vertical = baseline;
    vertical.impactAngleDegrees = 0.0;
    auto grazing = baseline;
    grazing.impactAngleDegrees = 89.0;
    Require(terrain_impacts::ScaleImpactCraterRadiusMeters(vertical) > normal &&
            normal > terrain_impacts::ScaleImpactCraterRadiusMeters(grazing),
        "Angles measured from the normal must give vertical impacts the largest radius.");

    const auto planet = MakePlanet();
    auto definition = EmptyDefinition(planet, 0x4500ULL);
    auto impact = MakeImpact(0x4501ULL, {0.0, 1.0, 0.0}, 0.0);
    impact.impactorDiameterMeters = baseline.impactorDiameterMeters;
    impact.impactVelocityMetersPerSecond = baseline.impactVelocityMetersPerSecond;
    impact.impactorDensityKgPerCubicMeter = baseline.impactorDensityKgPerCubicMeter;
    impact.impactAngleDegrees = baseline.impactAngleDegrees;
    definition.authoredImpacts.push_back(impact);
    terrain_impacts::ImpactField field(planet, definition);
    RequireNear(field.ResolvedImpacts().front().radiusMeters, normal, 1.0e-8,
        "Authored impactor parameters must compile into the crater radius used by terrain sampling.");
    definition.authoredImpacts.front().impactAngleDegrees = 0.0;
    const terrain_impacts::ImpactField verticalField(planet, definition);
    RequireNear(verticalField.ResolvedImpacts().front().radiusMeters,
        terrain_impacts::ScaleImpactCraterRadiusMeters(vertical), 1.0e-8,
        "A default vertical authored impact must use exactly the same scaling convention.");
}

void TestLongIrregularRaysAndRegionalBounds()
{
    const auto planet = MakePlanet();
    auto definition = EmptyDefinition(planet, 0x4510ULL);
    const math::Double3 center = math::Normalize(math::Double3{0.7, 0.2, 0.68});
    auto impact = MakeImpact(0x4511ULL, center, 20'000.0);
    impact.rayStrength = 1.2;
    impact.rayCount = 7U;
    impact.rayExtentRadii = 16.0;
    impact.rayIrregularity = 0.2;
    definition.authoredImpacts.push_back(impact);
    const terrain_impacts::ImpactField field(planet, definition);
    auto legacyDefinition = definition;
    legacyDefinition.authoredImpacts.front().rayExtentRadii = 0.0;
    legacyDefinition.authoredImpacts.front().rayIrregularity = 0.0;
    const terrain_impacts::ImpactField legacy(planet, legacyDefinition);
    const auto frame = world::MakeSurfaceFrame(center);
    terrain_impacts::ImpactQueryScratch scratch;
    std::vector<terrain_impacts::GeologicalEventReference> batch;
    f64 maximumRay = 0.0;
    f64 minimumRay = 1.0;
    math::Double3 brightestDirection{};
    for (u32 index = 0U; index < 128U; ++index)
    {
        const f64 azimuth = 2.0 * std::numbers::pi_v<f64> * index / 128.0;
        const auto direction = world::DirectionAtSurfaceOffset(planet, frame,
            {std::cos(azimuth) * impact.radiusMeters * 10.0,
             std::sin(azimuth) * impact.radiusMeters * 10.0});
        const auto sample = field.Sample(direction, 50.0, scratch);
        RequireNear(sample.heightDeltaMeters, 0.0, 0.0,
            "Distant material rays must not extend the massive ejecta relief.");
        RequireNear(sample.ejectaThicknessMeters, 0.0, 0.0,
            "Distant material rays must not invent a massive ejecta deposit.");
        RequireNear(legacy.Sample(direction, 50.0).rayField, 0.0, 0.0,
            "Recipes without an extended ray field must retain legacy finite support.");
        field.CollectEventsIntersectingCap(direction, 0.0, scratch, batch);
        Require(batch.size() == 1U,
            "A distant ray tile must include its source crater in the spatial batch.");
        RequireNear(field.Sample(direction, 50.0, scratch, batch).rayField,
            sample.rayField, 1.0e-12,
            "Tile batching must preserve rays beyond the ejecta blanket.");
        minimumRay = std::min(minimumRay, sample.rayField);
        if (sample.rayField > maximumRay)
        {
            maximumRay = sample.rayField;
            brightestDirection = direction;
        }
    }
    Require(maximumRay > 0.2 && minimumRay < 0.001,
        "Extended rays should be isolated spokes with gaps instead of a bright disk.");

    const auto caps = terrain_impacts::ChangedAuthoredEventInfluenceCaps(
        planet, legacyDefinition, definition);
    Require(caps.has_value() && caps->size() == 2U,
        "Ray edits should invalidate the union of their old and new regional support.");
    const f64 rayAngle = std::acos(std::clamp(math::Dot(center, brightestDirection), -1.0, 1.0));
    Require(caps->back().angularRadiusRadians > rayAngle,
        "Invalidation must cover the extended rays beyond the old crater support.");

    const auto rayFrame = world::MakeSurfaceFrame(brightestDirection);
    definition.resurfacingEvents.push_back({
        .id = ImpactId(0x4512ULL),
        .centerlineUnitDirections = {
            world::DirectionAtSurfaceOffset(planet, rayFrame, {-2'000.0, 0.0}),
            world::DirectionAtSurfaceOffset(planet, rayFrame, {2'000.0, 0.0})},
        .widthMeters = 2'000.0,
        .thicknessMeters = 100.0,
        .ageOrder = impact.ageOrder + 1U});
    const terrain_impacts::ImpactField buried(planet, definition);
    RequireNear(buried.Sample(brightestDirection, 50.0).rayField, 0.0, 0.0,
        "Younger resurfacing must bury distant material rays through the same chronology.");

    const auto outside = world::DirectionAtSurfaceOffset(
        planet, frame, {impact.radiusMeters * 30.0, 0.0});
    Require(field.CandidateCount(outside) == 0U &&
            field.Sample(outside, 50.0).affectingImpacts == 0U,
        "Extended rays must still have bounded, spatially pruned support.");
}

void TestRayRecipeValidationAndLegacyDefaults()
{
    const auto planet = MakePlanet();
    auto definition = EmptyDefinition(planet, 0x4520ULL);
    auto impact = MakeImpact(0x4521ULL, {0.0, 1.0, 0.0}, 20'000.0);
    for (const f64 invalidExtent : {-1.0, 0.5, 1.0, 101.0})
    {
        impact.rayExtentRadii = invalidExtent;
        Require(!impact.IsValid(), "Ray extents must be zero or in (1, 100].");
    }
    impact.rayExtentRadii = 12.0;
    impact.rayIrregularity = 1.1;
    Require(!impact.IsValid(), "Ray irregularity must stay in its normalized range.");
    impact.rayIrregularity = 0.3;
    impact.rayStrength = 1.0;
    impact.rayCount = 8U;
    definition.authoredImpacts.push_back(impact);
    const auto encoded = terrain_impacts::SerializeImpactFieldToml(definition);
    const auto decoded = terrain_impacts::ParseImpactFieldToml(encoded);
    Require(decoded.authoredImpacts.front().rayExtentRadii == 12.0 &&
            decoded.authoredImpacts.front().rayIrregularity == 0.3,
        "New ray modifiers must survive project-authority serialization.");
    auto oldRecipe = encoded;
    for (const std::string key : {"ray_extent_radii", "ray_irregularity"})
    {
        const auto start = oldRecipe.find(key + " =");
        Require(start != std::string::npos, "The ray field must be serialized.");
        oldRecipe.erase(start, oldRecipe.find('\n', start) - start + 1U);
    }
    const auto old = terrain_impacts::ParseImpactFieldToml(oldRecipe);
    Require(old.authoredImpacts.front().rayExtentRadii == 0.0 &&
            old.authoredImpacts.front().rayIrregularity == 0.0,
        "Old recipes must default to unchanged ray support and regularity.");
}

void TestFreshResurfacingClearsStatisticalMicrocraters()
{
    const auto planet = MakePlanet();
    auto definition = EmptyDefinition(planet, 0x4530ULL);
    definition.surfaceAgeYears = 100'000'000.0;
    definition.procedural.count = 200'000U;
    definition.procedural.minimumRadiusMeters = 100.0;
    const math::Double3 center{0.0, 1.0, 0.0};
    const auto frame = world::MakeSurfaceFrame(center);
    definition.resurfacingEvents.push_back({
        .id = ImpactId(0x4531ULL),
        .centerlineUnitDirections = {
            world::DirectionAtSurfaceOffset(planet, frame, {-20'000.0, 0.0}),
            world::DirectionAtSurfaceOffset(planet, frame, {20'000.0, 0.0})},
        .widthMeters = 50'000.0,
        .thicknessMeters = 200.0,
        .formationAgeYears = definition.surfaceAgeYears,
        .ageOrder = 1ULL << 40U});
    const terrain_impacts::ImpactField field(planet, definition);
    const auto fresh = field.Sample(center, 1'000.0);
    Require(fresh.exposureAgeYears == 0.0 && fresh.resurfacedMaterialFraction == 1.0 &&
            fresh.microImpactCoverage == 0.0 && fresh.microImpactRoughnessMeters == 0.0,
        "Zero exposure age after fresh lava must not restore an ancient micro-impact population.");
    const auto untouched = field.Sample(center * -1.0, 1'000.0);
    Require(untouched.microImpactCoverage > 0.0,
        "Fresh resurfacing must leave the distant statistical population intact.");
    const auto& events = field.ResolvedImpacts();
    Require(events.front().formationAgeYears > 0.0 &&
            events.back().formationAgeYears < definition.surfaceAgeYears &&
            events.front().formationAgeYears < events.back().formationAgeYears,
        "Procedural impacts must span the history instead of all forming at its start.");
}

void TestBinaryAndSecondaryCraterEvents()
{
    const auto planet = MakePlanet();
    const math::Double3 center = math::Normalize(math::Double3{-0.3, 0.8, 0.4});
    auto definition = EmptyDefinition(planet, 0x4600ULL);
    auto primary = MakeImpact(0x4601ULL, center, 24'000.0);
    primary.binarySeparationRadii = 2.0;
    primary.binaryCompanionRadiusRatio = 0.55;
    primary.binaryAzimuthRadians = 0.4;
    primary.secondaryCount = 3U;
    primary.rayCount = 4U;
    primary.rayStrength = 0.8;
    definition.authoredImpacts.push_back(primary);
    terrain_impacts::ImpactField field(planet, definition);
    Require(field.ResolvedImpacts().size() == 5U,
        "One binary event and three secondary craters should compile into five indexed primitives.");

    constexpr f64 footprint = 50.0;
    u32 derived = 0U;
    for (const auto& event : field.ResolvedImpacts())
    {
        if (event.authored) continue;
        ++derived;
        const auto sample = field.Sample(Position(planet, event.centerUnitDirection), footprint);
        Require(sample.heightDeltaMeters < 0.0,
            "Every generated binary/secondary cavity should contribute terrain relief.");
    }
    Require(derived == 4U,
        "Binary and secondary primitives should remain deterministic derived events.");
}

void TestStressGuidedIceFractureCurves()
{
    const auto planet = MakePlanet();
    terrain_impacts::IceFractureDefinition definition{
        .seed = 0x88776655ULL,
        .ageOrder = 17U,
        .formationAgeYears = 1'000'000'000.0,
        .enabled = true,
        .tidalAxis = {1.0, 0.0, 0.0},
        .spinAxis = {0.0, 1.0, 0.0},
        .tidalStress = 0.0,
        .rotationalStress = 0.0,
        .tensileStrength = 0.0,
        .fractureCount = 1U,
        .segmentsPerFracture = 4U,
        .maximumLengthMeters = 40'000.0,
        .widthMeters = 2'000.0,
        .grooveDepthMeters = 80.0,
        .ridgeHeightMeters = 20.0,
        .branchProbability = 0.0
    };
    terrain_impacts::IceFractureField field(planet, definition);
    Require(field.SegmentCount() == 4U,
        "A fracture path should compile into the configured spherical curve segments.");
    Require(field.Segments().size() == field.SegmentCount(),
        "The immutable fracture segment batch must expose the indexed geometry.");

    const auto mix = [](u64 value)
    {
        value += 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    };
    const auto unit = [&](u64 value)
    {
        return static_cast<f64>(mix(value) >> 11U) /
            static_cast<f64>(1ULL << 53U);
    };
    const f64 u = unit(definition.seed ^ 0x9E3779B97F4A7C15ULL);
    const f64 v = unit(definition.seed ^ 0xBF58476D1CE4E5B9ULL);
    const f64 z = 1.0 - 2.0 * u;
    const f64 radial = std::sqrt(std::max(0.0, 1.0 - z * z));
    const math::Double3 start{
        radial * std::cos(2.0 * std::numbers::pi_v<f64> * v), z,
        radial * std::sin(2.0 * std::numbers::pi_v<f64> * v)};
    terrain_impacts::ImpactQueryScratch scratch;
    const auto sample = field.Sample(
        Position(planet, start),
        100.0, scratch);
    Require(sample.damage > 0.0 && std::isfinite(sample.heightDeltaMeters) &&
            sample.ageOrder == 17U && sample.formationAgeYears == 1'000'000'000.0,
        "Indexed fracture curves should produce finite relief, damage and chronology at the seeded trace.");
    std::vector<std::size_t> segmentBatch;
    field.CollectSegmentsIntersectingCap(
        start, 0.05, scratch, segmentBatch);
    Require(segmentBatch.size() == field.SegmentCount(),
        "A fracture tile batch should gather the nearby indexed segments.");
    const auto batchedSample = field.Sample(start, 100.0, segmentBatch);
    RequireNear(batchedSample.heightDeltaMeters, sample.heightDeltaMeters, 1.0e-6,
        "A conservative fracture batch must preserve the canonical relief sample.");
    Require(batchedSample.nearbySegments == sample.nearbySegments &&
            batchedSample.ageOrder == sample.ageOrder,
        "A conservative fracture batch must preserve damage support and chronology.");
    field.CollectSegmentsIntersectingCap(
        start * -1.0, 0.05, scratch, segmentBatch);
    Require(segmentBatch.empty(),
        "A fracture tile batch should prune segments outside its spherical influence cap.");
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
    const world::SurfaceFrame frame = world::MakeSurfaceFrame(center);

    constexpr f64 footprint = 50.0;

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

        const auto samplePosition = world::DirectionAtSurfaceOffset(
            planet, frame,
            {std::cos(angle) * distance, std::sin(angle) * distance});

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
    authored.formationAgeYears = 2.4e9;
    authored.impactAngleDegrees = 42.0;
    authored.impactAzimuthRadians = 1.3;
    authored.shapeIrregularity = 0.14;
    authored.meltFraction = 0.35;
    authored.brecciaFraction = 0.75;
    authored.multiringStrength = 0.2;
    authored.binarySeparationRadii = 1.7;
    authored.binaryCompanionRadiusRatio = 0.42;
    authored.binaryAzimuthRadians = 2.1;
    authored.secondaryCount = 2U;
    authored.secondaryRadiusRatio = 0.09;
    authored.secondaryRayAlignment = 0.6;
    definition.environment = terrain_impacts::SurfaceEnvironment::Icy;
    definition.surfaceAgeYears = 4.0e9;
    definition.iceFractures = std::make_shared<terrain_impacts::IceFractureDefinition>(
        terrain_impacts::IceFractureDefinition{
            .seed = 0x88776655ULL,
            .ageOrder = 0xAABBCCDDEEFF0011ULL,
            .formationAgeYears = 1.5e9,
            .tidalAxis = {0.2, 0.9, -0.1},
            .spinAxis = {0.0, 1.0, 0.0},
            .tidalStress = 0.8,
            .rotationalStress = 0.12,
            .tensileStrength = 0.18,
            .fractureCount = 37U,
            .segmentsPerFracture = 9U,
            .maximumLengthMeters = 850'000.0,
            .widthMeters = 2'300.0,
            .grooveDepthMeters = 120.0,
            .ridgeHeightMeters = 42.0,
            .branchProbability = 0.24
        });
    definition.authoredImpacts.push_back(authored);
    const math::Double3 flowCenter = math::Normalize(math::Double3{0.3, 0.8, -0.4});
    const auto flowFrame = world::MakeSurfaceFrame(flowCenter);
    terrain_impacts::ResurfacingRecord flow{
        .id = ImpactId(0x99887766ULL),
        .kind = terrain_impacts::ResurfacingKind::LavaFlow,
        .centerlineUnitDirections = {
            world::DirectionAtSurfaceOffset(planet, flowFrame, {-35'000.0, 0.0}),
            flowCenter,
            world::DirectionAtSurfaceOffset(planet, flowFrame, {35'000.0, 0.0})},
        .widthMeters = 18'000.0,
        .thicknessMeters = 240.0,
        .formationAgeYears = 900'000.0,
        .ageOrder = 1'300'000'000'000'000'000ULL
    };
    definition.resurfacingEvents.push_back(flow);
    auto faultRenewal = flow;
    faultRenewal.id = ImpactId(0x99887767ULL);
    faultRenewal.kind = terrain_impacts::ResurfacingKind::TectonicRenewal;
    faultRenewal.displacementUnitDirection = flowFrame.east;
    faultRenewal.displacementMeters = 3'200.0;
    faultRenewal.ageOrder += 1U;
    definition.resurfacingEvents.push_back(faultRenewal);
    auto plateMotion = faultRenewal;
    plateMotion.id = ImpactId(0x99887768ULL);
    plateMotion.centerlineUnitDirections = {
        world::DirectionAtSurfaceOffset(planet, flowFrame, {-100'000.0, -100'000.0}),
        world::DirectionAtSurfaceOffset(planet, flowFrame, {100'000.0, -100'000.0}),
        world::DirectionAtSurfaceOffset(planet, flowFrame, {100'000.0, 100'000.0}),
        world::DirectionAtSurfaceOffset(planet, flowFrame, {-100'000.0, 100'000.0}),
        world::DirectionAtSurfaceOffset(planet, flowFrame, {-100'000.0, -100'000.0})};
    plateMotion.displacementMeters = 1'800.0;
    plateMotion.regionalPlateMotion = true;
    plateMotion.ageOrder += 1U;
    definition.resurfacingEvents.push_back(plateMotion);

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
    Require(decoded.resurfacingEvents.size() == 3U &&
            decoded.resurfacingEvents.front().id == flow.id &&
            decoded.resurfacingEvents.front().kind == flow.kind &&
            decoded.resurfacingEvents.front().centerlineUnitDirections.size() == 3U &&
            decoded.resurfacingEvents.front().widthMeters == flow.widthMeters &&
            decoded.resurfacingEvents.front().thicknessMeters == flow.thicknessMeters &&
            decoded.resurfacingEvents.front().ageOrder == flow.ageOrder,
        "Connected resurfacing paths must survive TOML round-trip.");
    Require(decoded.resurfacingEvents[1].id == faultRenewal.id &&
            decoded.resurfacingEvents[1].kind ==
                terrain_impacts::ResurfacingKind::TectonicRenewal &&
            decoded.resurfacingEvents[1].displacementMeters == 3'200.0 &&
            math::LengthSquared(decoded.resurfacingEvents[1].displacementUnitDirection -
                faultRenewal.displacementUnitDirection) < 1.0e-12 &&
            decoded.resurfacingEvents.back().id == plateMotion.id &&
            decoded.resurfacingEvents.back().regionalPlateMotion &&
            math::LengthSquared(decoded.resurfacingEvents.back().displacementUnitDirection -
                plateMotion.displacementUnitDirection) < 1.0e-12 &&
            decoded.resurfacingEvents.back().ageOrder == plateMotion.ageOrder,
        "Tectonic renewal chronology must survive TOML round-trip.");

    const auto& crater =
        decoded.authoredImpacts.front();

    Require(
        crater.id == authored.id &&
        crater.profile == authored.profile &&
        crater.rayCount == authored.rayCount &&
        crater.ageOrder == authored.ageOrder &&
        crater.impactAngleDegrees == authored.impactAngleDegrees &&
        crater.impactAzimuthRadians == authored.impactAzimuthRadians &&
        crater.shapeIrregularity == authored.shapeIrregularity &&
        crater.meltFraction == authored.meltFraction &&
        crater.brecciaFraction == authored.brecciaFraction &&
        crater.multiringStrength == authored.multiringStrength &&
        crater.binarySeparationRadii == authored.binarySeparationRadii &&
        crater.binaryCompanionRadiusRatio == authored.binaryCompanionRadiusRatio &&
        crater.binaryAzimuthRadians == authored.binaryAzimuthRadians &&
        crater.secondaryCount == authored.secondaryCount &&
        crater.secondaryRadiusRatio == authored.secondaryRadiusRatio &&
        crater.secondaryRayAlignment == authored.secondaryRayAlignment &&
        decoded.environment == definition.environment &&
        decoded.surfaceAgeYears == definition.surfaceAgeYears &&
        decoded.iceFractures != nullptr &&
        decoded.iceFractures->seed == definition.iceFractures->seed &&
        decoded.iceFractures->ageOrder == definition.iceFractures->ageOrder &&
        decoded.iceFractures->formationAgeYears == definition.iceFractures->formationAgeYears &&
        decoded.iceFractures->fractureCount == definition.iceFractures->fractureCount &&
        decoded.iceFractures->segmentsPerFracture ==
            definition.iceFractures->segmentsPerFracture &&
        decoded.iceFractures->tidalAxis == definition.iceFractures->tidalAxis,
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

    constexpr f64 footprint = 100.0;
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

    const auto resurfaced = original.Sample(flowCenter, footprint);
    Require(resurfaced.resurfacedMaterialFraction > 0.99 &&
            resurfaced.resurfacingThicknessMeters > 180.0 &&
            resurfaced.exposureAgeOrder == faultRenewal.ageOrder &&
            resurfaced.formationAgeYears == flow.formationAgeYears,
        "A connected lava flow must cover older relief, and later fault renewal must advance only the local exposure age.");
}
} // namespace

int main()
{
    TestMoonPresetIsDeterministicAndCraterDominated();
    TestSpatialIndexAndReusableScratch();
    TestTenMillionPopulationUsesStatisticalMicrocraters();
    TestSimpleAndComplexProfiles();
    TestOverlapModifiesPriorCrater();
    BenchmarkThousandOverlappingImpacts();
    TestImpactsAndResurfacingShareChronology();
    TestTectonicRenewalDisplacesOlderImpacts();
    TestChronologicalEventBatchReferences();
    TestDegradationCanSoftenAndFillCraterRelief();
    TestEnvironmentAgeAndObliqueMorphology();
    TestImpactScalingUsesGravityAndTargetStrength();
    TestLongIrregularRaysAndRegionalBounds();
    TestRayRecipeValidationAndLegacyDefaults();
    TestFreshResurfacingClearsStatisticalMicrocraters();
    TestBinaryAndSecondaryCraterEvents();
    TestStressGuidedIceFractureCurves();
    TestEjectaRaysAndDebrisFields();
    TestAuthoredImpactRoundTrip();

    std::cout << "Orbit M07 impact/crater tests passed.\n";
    return EXIT_SUCCESS;
}
