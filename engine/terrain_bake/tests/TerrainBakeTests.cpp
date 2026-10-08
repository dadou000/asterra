#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_bake/PlanetBakeFile.hpp>
#include <orbit/terrain_bake/RiverBaker.hpp>
#include <orbit/terrain_bake/TectonicBaker.hpp>
#include <orbit/terrain_bake/TerrainBakeService.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace orbit;

bool Check(const bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

math::Double3 Fibonacci(const u32 i, const u32 n)
{
    const f64 y = 1.0 - 2.0 * (static_cast<f64>(i) + 0.5) / static_cast<f64>(n);
    const f64 r = std::sqrt(std::max(0.0, 1.0 - y * y));
    const f64 a = std::numbers::pi * (3.0 - std::sqrt(5.0)) * static_cast<f64>(i);
    return {r * std::cos(a), y, r * std::sin(a)};
}

world::PlanetDefinition MakePlanet()
{
    return {.radiusMeters = 6'371'000.0,
            .id = {.high = 0x4F5242414B450001ULL, .low = 0x1ULL},
            .generationSeed = 0x1234ULL};
}

terrain::AnalyticTerrainDesc MakeDesc(const u64 seed = 4242)
{
    terrain::AnalyticTerrainDesc desc{.seed = seed};
    return desc;
}

std::filesystem::path TempDirectory(const char* name)
{
    const auto path = std::filesystem::temp_directory_path() / "orbit_bake_tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
    return path;
}

bool BakeIsDeterministicAndResolutionConsistent()
{
    // Plate ownership comes from noise-metric growth over the raster, so there
    // is no closed-form truth to compare with. The bake must be deterministic,
    // and a finer raster must describe the same planet: the layers agree away
    // from the resolution-limited fine detail, and plate identity agrees
    // almost everywhere.
    const auto planet = MakePlanet();
    const auto desc = MakeDesc();
    const auto coarse = terrain_bake::BakeTectonics(planet, desc, {.resolution = 96});
    const auto again = terrain_bake::BakeTectonics(planet, desc, {.resolution = 96});
    const auto fine = terrain_bake::BakeTectonics(planet, desc, {.resolution = 192});
    bool ok = Check(coarse && again && fine, "bakes must complete");
    if (!ok) return false;
    ok &= Check(coarse->ContentHash() == again->ContentHash(), "the bake must be deterministic");

    f64 sumConvergence = 0.0;
    f64 sumAge = 0.0;
    u32 plateMismatches = 0;
    std::vector<f64> convergenceErrors;
    constexpr u32 count = 20000;
    for (u32 i = 0; i < count; ++i)
    {
        const auto d = Fibonacci(i, count);
        const auto a = coarse->Sample(d);
        const auto b = fine->Sample(d);
        const f64 e = std::abs(static_cast<f64>(
            a.Get(terrain::BakedTectonicLayer::Convergence) - b.Get(terrain::BakedTectonicLayer::Convergence)));
        sumConvergence += e;
        convergenceErrors.push_back(e);
        sumAge += std::abs(static_cast<f64>(
            a.Get(terrain::BakedTectonicLayer::CrustAge) - b.Get(terrain::BakedTectonicLayer::CrustAge)));
        plateMismatches += a.plate != b.plate ? 1U : 0U;
    }
    std::sort(convergenceErrors.begin(), convergenceErrors.end());
    std::cout << "bake @96 vs @192: mean convergence diff " << sumConvergence / count
              << ", p99 " << convergenceErrors[count * 99 / 100] << ", mean age diff "
              << sumAge / count << ", plate id mismatches " << plateMismatches << " of " << count
              << std::endl;
    ok &= Check(sumConvergence / count < 0.03, "bake layers must agree across resolutions on average");
    ok &= Check(convergenceErrors[count * 99 / 100] < 0.45, "bake layers must not differ wildly across resolutions");
    ok &= Check(plateMismatches < count / 25U, "plate identity must agree across resolutions");
    return ok;
}

bool BakedFieldsAreContinuousAcrossCubeEdges()
{
    // Texels near a cube edge interpolate through the gutter. Their local
    // variation must be no worse than anywhere else on the planet: no seam.
    const auto planet = MakePlanet();
    const auto desc = MakeDesc();
    const auto bake = terrain_bake::BakeTectonics(planet, desc, {.resolution = 96});
    if (!Check(bake != nullptr, "bake must complete")) return false;

    const auto worstJump = [&](const auto& pointAt)
    {
        f64 worst = 0.0;
        for (u32 i = 0; i < 6000; ++i)
        {
            const math::Double3 d = pointAt(static_cast<f64>(i) / 6000.0, i);
            const math::Double3 e = math::Normalize(d + math::Double3{1.5e-3, 0.7e-3, -1.1e-3});
            const auto a = bake->Sample(d);
            const auto b = bake->Sample(e);
            for (const auto layer : {terrain::BakedTectonicLayer::Convergence,
                     terrain::BakedTectonicLayer::Divergence,
                     terrain::BakedTectonicLayer::Transform,
                     terrain::BakedTectonicLayer::ContinentalCrustFraction})
            {
                worst = std::max(worst, std::abs(static_cast<f64>(a.Get(layer) - b.Get(layer))));
            }
        }
        return worst;
    };
    // Points within a fraction of a texel of the +X/+Z and +Y/-Z cube edges.
    const auto edgePoints = [](const f64 t, const u32 i)
    {
        const f64 jitter = (static_cast<f64>(i % 7U) - 3.0) * 4.0e-3;
        if (i % 2U == 0U)
        {
            const f64 y = -0.9 + 1.8 * t;
            const f64 e = std::sqrt((1.0 - y * y) * 0.5);
            return math::Normalize(math::Double3{e + jitter, y, e - jitter});
        }
        const f64 x = -0.9 + 1.8 * t;
        const f64 e = std::sqrt((1.0 - x * x) * 0.5);
        return math::Normalize(math::Double3{x, e + jitter, -(e - jitter)});
    };
    const f64 edge = worstJump(edgePoints);
    const f64 anywhere = worstJump([](const f64, const u32 i) { return Fibonacci(i, 6000U); });
    std::cout << "worst local jump: near cube edges " << edge << ", anywhere " << anywhere << std::endl;
    return Check(edge <= anywhere * 1.5 + 0.05,
        "baked layers must be as smooth across cube-face edges as inside a face");
}

bool FileRoundTripIsExactAndCorruptionIsRejected()
{
    const auto planet = MakePlanet();
    const auto bake = terrain_bake::BakeTectonics(planet, MakeDesc(), {.resolution = 48});
    if (!Check(bake != nullptr, "bake must complete")) return false;

    const auto dir = TempDirectory("roundtrip");
    const auto path = dir / "planet.orbitbake";
    terrain_bake::SavePlanetBake(path, {.tectonics = bake});

    std::string error;
    auto loaded = terrain_bake::LoadPlanetBake(path, &error);
    bool ok = Check(loaded.has_value() && loaded->tectonics != nullptr, "saved bake must load");
    if (!ok) return false;
    ok &= Check(loaded->tectonics->ContentHash() == bake->ContentHash(), "content hash must survive a round trip");
    ok &= Check(loaded->tectonics->RecipeHash() == bake->RecipeHash(), "recipe hash must survive a round trip");
    for (u32 i = 0; i < 500; ++i)
    {
        const auto d = Fibonacci(i, 500);
        const auto a = bake->Sample(d);
        const auto b = loaded->tectonics->Sample(d);
        ok &= Check(a.values == b.values && a.plate == b.plate,
            "a loaded bake must sample identically");
    }

    // Flip a payload byte: the section checksum must reject it.
    {
        std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(400);
        char byte = 0;
        f.read(&byte, 1);
        f.seekp(400);
        byte = static_cast<char>(byte ^ 0x5A);
        f.write(&byte, 1);
    }
    ok &= Check(!terrain_bake::LoadPlanetBake(path, &error).has_value(), "a corrupt bake must be rejected");

    // Truncation and a missing file are reported, not thrown.
    std::filesystem::resize_file(path, 100);
    ok &= Check(!terrain_bake::LoadPlanetBake(path, &error).has_value(), "a truncated bake must be rejected");
    ok &= Check(!terrain_bake::LoadPlanetBake(dir / "missing.orbitbake", &error).has_value(),
        "a missing bake must be reported");
    return ok;
}

bool BakedTerrainTracksTheProceduralTerrain()
{
    const auto planet = MakePlanet();
    auto desc = MakeDesc();
    const auto bake = terrain_bake::BakeTectonics(planet, desc, {.resolution = 256});
    if (!Check(bake != nullptr, "bake must complete")) return false;

    auto bakedDesc = desc;
    bakedDesc.global.bakedTectonics = bake;
    const terrain::AnalyticTerrainSource procedural(planet, desc);
    const terrain::AnalyticTerrainSource baked(planet, bakedDesc);

    bool ok = Check(procedural.Revision() != baked.Revision(),
        "attaching a bake must change the terrain revision so caches invalidate");

    f64 sumAbs = 0.0;
    f64 worst = 0.0;
    constexpr u32 count = 4000;
    for (u32 i = 0; i < count; ++i)
    {
        const terrain::TerrainQuery query{
            .unitDirection = Fibonacci(i, count),
            .footprintMeters = 20'000.0,
            .planet = planet.id,
            .radialOffsetMeters = 0.0};
        const f64 a = procedural.Sample(query).elevationMeters;
        const f64 b = baked.Sample(query).elevationMeters;
        sumAbs += std::abs(a - b);
        worst = std::max(worst, std::abs(a - b));
    }
    std::cout << "baked vs procedural elevation: mean |diff| " << sumAbs / count
              << " m, worst " << worst << " m" << std::endl;
    // The bake has its own geography (grown plates, crust-type driven bias),
    // orogenic belts that exist at zero noise and the structural elevation, so
    // it agrees with the unbaked plate model in scale, not point by point.
    ok &= Check(sumAbs / count < 2000.0, "baked terrain must stay at the procedural terrain's scale");
    ok &= Check(worst < 12000.0, "baked terrain must not deviate wildly anywhere");

    // The structure reaches the terrain: the same bake with its structural
    // elevation layer zeroed is the control. Trenches must be lower with the
    // structure than without, and volcanic arcs higher.
    {
        std::array<std::vector<u16>, terrain::kBakedTectonicLayerCount> layers;
        std::array<f32, terrain::kBakedTectonicLayerCount> minimum{}, maximum{};
        for (u32 l = 0; l < terrain::kBakedTectonicLayerCount; ++l)
        {
            const auto id = static_cast<terrain::BakedTectonicLayer>(l);
            layers[l] = bake->QuantizedLayer(id);
            minimum[l] = bake->RangeMinimum(id);
            maximum[l] = bake->RangeMaximum(id);
        }
        const auto structural = static_cast<std::size_t>(terrain::BakedTectonicLayer::StructuralElevationMeters);
        const f64 zeroCode = (0.0 - minimum[structural]) / (static_cast<f64>(maximum[structural]) - minimum[structural]) * 65535.0;
        std::fill(layers[structural].begin(), layers[structural].end(), static_cast<u16>(std::clamp(zeroCode + 0.5, 0.0, 65535.0)));
        auto flatDesc = desc;
        flatDesc.global.bakedTectonics = std::make_shared<const terrain::BakedTectonicRasters>(
            terrain::BakedTectonicRasters::FromQuantized(
                bake->Resolution(), bake->RecipeHash(), std::move(layers), minimum, maximum,
                bake->PlateIds(), bake->NeighbourIds(), bake->PlateContinentalFlags(), bake->PlateCount()));
        const terrain::AnalyticTerrainSource control(planet, flatDesc);

        f64 trenchSum = 0.0, arcSum = 0.0;
        u32 trenchCount = 0, arcCount = 0;
        for (u32 i = 0; i < 80'000; ++i)
        {
            const math::Double3 direction = Fibonacci(i, 80'000);
            const auto structure = baked.GlobalFields().SampleTectonicStructure(direction);
            const bool trench = structure.subductionTrench > 0.6;
            const bool arc = structure.volcanicArc > 0.6;
            if (!trench && !arc) continue;
            const terrain::TerrainQuery query{
                .unitDirection = direction, .footprintMeters = 20'000.0,
                .planet = planet.id, .radialOffsetMeters = 0.0};
            const f64 delta = baked.Sample(query).elevationMeters - control.Sample(query).elevationMeters;
            if (trench) { trenchSum += delta; ++trenchCount; }
            else { arcSum += delta; ++arcCount; }
        }
        const f64 trenchMean = trenchCount ? trenchSum / trenchCount : 0.0;
        const f64 arcMean = arcCount ? arcSum / arcCount : 0.0;
        std::cout << "structure vs zeroed-structure control: trench " << trenchMean << " m (" << trenchCount
                  << "), arc " << arcMean << " m (" << arcCount << ")" << std::endl;
        ok &= Check(trenchCount > 20U && trenchMean < -800.0,
            "trenches must reach the baked terrain as depressions");
        ok &= Check(arcCount > 5U && arcMean > 100.0,
            "volcanic arcs must reach the baked terrain as uplift");
    }

    // The structural layer is served from the bake too.
    const auto structure = baked.GlobalFields().SampleTectonicStructure(Fibonacci(7, 100));
    ok &= Check(structure.crustThicknessKm >= 3.0 && std::isfinite(structure.crustThicknessKm),
        "baked structure samples must be valid");
    return ok;
}

bool RecipeHashTracksOnlyBakedInputs()
{
    const auto planet = MakePlanet();
    auto desc = MakeDesc();
    const u64 base = terrain::TectonicBakeRecipeHash(planet, desc);
    bool ok = true;

    auto plates = desc;
    plates.global.tectonic.plateCount += 1;
    ok &= Check(terrain::TectonicBakeRecipeHash(planet, plates) != base, "plate count must change the recipe");

    auto uplift = desc;
    uplift.global.tectonic.convergenceUpliftMeters += 1.0;
    ok &= Check(terrain::TectonicBakeRecipeHash(planet, uplift) != base, "convergence uplift must change the recipe");

    auto hotspots = desc;
    hotspots.global.tectonic.hotspotCount = 2;
    hotspots.global.tectonic.hotspotBaseReliefMeters += 100.0;
    hotspots.global.tectonic.rainShadowStrength += 0.1;
    ok &= Check(terrain::TectonicBakeRecipeHash(planet, hotspots) == base,
        "hotspot and rain-shadow edits are not baked and must not make a bake stale");

    auto bigger = planet;
    bigger.radiusMeters *= 2.0;
    ok &= Check(terrain::TectonicBakeRecipeHash(bigger, desc) != base, "planet radius must change the recipe");
    return ok;
}

// Pumps the service until `done` or a timeout.
template <typename Predicate>
bool Pump(terrain_bake::TerrainBakeService& service, Predicate done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline)
    {
        service.Tick(0.5);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool ServiceBakesRebakesAndKeepsTheRunningBakeOnFailure()
{
    const auto planet = MakePlanet();
    auto desc = MakeDesc();
    const auto dir = TempDirectory("service");
    bool ok = true;

    {
        terrain_bake::TerrainBakeService service(dir);
        const terrain_bake::BakeSettings settings{.resolution = 32, .autoRebake = true};
        service.Observe(planet.id, planet, desc, settings);
        ok &= Check(service.Status(planet.id).state == terrain_bake::BakeState::None,
            "an unbaked planet starts with no bake");

        ok &= Check(Pump(service, [&] { return service.Status(planet.id).state == terrain_bake::BakeState::Ready; }),
            "the service must bake a missing bake automatically");
        auto completed = service.TakeCompleted();
        ok &= Check(completed.size() == 1 && completed[0].matchesRecipe, "the finished bake is handed over once");
        const auto first = service.Active(planet.id);
        ok &= Check(first != nullptr && std::filesystem::exists(service.BakePath(planet.id)),
            "the finished bake is active and saved");

        // Edit the recipe: the old bake stays active and Stale until the new
        // one finishes.
        auto edited = desc;
        edited.global.tectonic.plateCount = 9;
        service.Observe(planet.id, planet, edited, settings);
        ok &= Check(service.Status(planet.id).state == terrain_bake::BakeState::Stale,
            "an edited recipe makes the bake stale");
        ok &= Check(service.Active(planet.id) == first, "the old bake keeps running while stale");
        ok &= Check(Pump(service, [&] { return service.Status(planet.id).state == terrain_bake::BakeState::Ready; }),
            "a stale bake must rebuild in the background");
        ok &= Check(service.Active(planet.id) != first, "the new bake replaces the old one");
        ok &= Check(service.Active(planet.id)->RecipeHash() ==
                terrain::TectonicBakeRecipeHash(planet, edited),
            "the new bake matches the edited recipe");
        static_cast<void>(service.TakeCompleted());

        // A bake that fails must leave the running one alone.
        const auto running = service.Active(planet.id);
        auto again = edited;
        again.global.tectonic.plateCount = 11;
        service.Observe(planet.id, planet, again, {.resolution = 8, .autoRebake = true});
        ok &= Check(Pump(service, [&] { return service.Status(planet.id).state == terrain_bake::BakeState::Failed; }),
            "an invalid bake must fail rather than crash");
        ok &= Check(service.Active(planet.id) == running, "a failed bake must leave the active bake alive");
        ok &= Check(!service.Status(planet.id).error.empty(), "the failure must be reported");
        ok &= Check(service.TakeCompleted().empty(), "a failed bake hands nothing over");
    }

    // A new session reuses the saved bake without rebaking.
    {
        terrain_bake::TerrainBakeService service(dir);
        auto edited = desc;
        edited.global.tectonic.plateCount = 9;
        service.Observe(planet.id, planet, edited, {.resolution = 32, .autoRebake = true});
        const auto completed = service.TakeCompleted();
        ok &= Check(completed.size() == 1 && completed[0].matchesRecipe,
            "a saved bake for the current recipe loads at once");
        ok &= Check(service.Status(planet.id).state == terrain_bake::BakeState::Ready,
            "and is ready without baking");
    }

    // Cancelling does not restart on its own.
    {
        terrain_bake::TerrainBakeService service(TempDirectory("cancel"));
        service.Observe(planet.id, planet, desc, {.resolution = 1024, .autoRebake = true});
        service.Tick(1.0);
        ok &= Check(service.Status(planet.id).state == terrain_bake::BakeState::Baking, "a large bake runs in the background");
        service.Cancel(planet.id);
        ok &= Check(Pump(service, [&] { return service.Status(planet.id).state == terrain_bake::BakeState::Failed; }),
            "a cancelled bake stops");
        ok &= Check(service.Active(planet.id) == nullptr, "and installs nothing");
    }
    return ok;
}
bool RiverBakeProducesAConsistentGraph()
{
    const auto planet = MakePlanet();
    auto desc = MakeDesc();
    desc.global.bakedTectonics = std::shared_ptr<const terrain::BakedTectonicRasters>(
        terrain_bake::BakeTectonics(planet, desc, {.resolution = 64}));
    const terrain_bake::RiverBakeOptions options{.resolution = 64, .minimumDischargeCubicMetersPerSecond = 200.0};
    const auto network = terrain_bake::BakeRivers(planet, desc, options);
    bool ok = Check(network != nullptr, "river bake completes");
    if (!ok) return false;
    ok &= Check(!network->Empty(), "an Earth-like planet has rivers");
    const auto& nodes = network->Nodes();
    const auto& segments = network->Segments();

    std::vector<i64> downstream(nodes.size(), -1);
    for (const auto& s : segments)
    {
        ok &= Check(downstream[s.upstream] < 0, "every node has one downstream reach");
        downstream[s.upstream] = s.downstream;
        ok &= Check(nodes[s.downstream].dischargeCubicMetersPerSecond + 1.0e-3F >=
                        nodes[s.upstream].dischargeCubicMetersPerSecond,
            "discharge never decreases downstream");
    }
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        std::size_t at = i;
        std::size_t steps = 0;
        while (downstream[at] >= 0 && steps++ <= nodes.size()) at = static_cast<std::size_t>(downstream[at]);
        ok &= Check(steps <= nodes.size(), "graph is acyclic");
        ok &= Check(nodes[at].elevationMeters <= desc.global.seaLevelMeters + 1.0, "every river ends at the sea");
        ok &= Check(nodes[i].basin == at, "basin is the mouth node");
    }

    // The carve is deepest on the centerline and zero far away.
    if (!segments.empty())
    {
        const auto& s = segments.front();
        const auto mid = math::Normalize(nodes[s.upstream].direction + nodes[s.downstream].direction);
        const auto carve = network->Sample(mid, 1.0);
        ok &= Check(carve.depthMeters > 0.0F, "carve on the centerline");
    }

    // Deterministic.
    const auto again = terrain_bake::BakeRivers(planet, desc, options);
    ok &= Check(again != nullptr && again->ContentHash() == network->ContentHash(), "river bake is deterministic");

    // A waterless planet yields an empty network.
    auto dry = desc;
    dry.global.seaLevelMeters = -1.0e6;
    const auto none = terrain_bake::BakeRivers(planet, dry, options);
    ok &= Check(none != nullptr && none->Empty(), "no ocean, no rivers");
    return ok;
}
} // namespace

bool BoundariesAreNaturalisedAtBakeTime()
{
    // The bake evaluates the deformed boundary structure: strike varies along
    // a boundary, plate boundaries are longer and more complex than the clean
    // plate model's, and fractures fill a corridor around them.
    const auto planet = MakePlanet();
    const auto desc = MakeDesc();
    const auto bake = terrain_bake::BakeTectonics(planet, desc, {.resolution = 128});
    if (!Check(bake != nullptr, "bake must complete")) return false;
    const terrain::AnalyticTerrainSource clean(planet, desc);

    u32 fractured = 0, farFractured = 0, far = 0;
    bool inRange = true;
    constexpr u32 count = 30'000;
    for (u32 i = 0; i < count; ++i)
    {
        const auto d = Fibonacci(i, count);
        const auto texel = bake->Sample(d);
        const f32 c = texel.Get(terrain::BakedTectonicLayer::Convergence);
        const f32 t = texel.Get(terrain::BakedTectonicLayer::Transform);
        const f32 e = texel.Get(terrain::BakedTectonicLayer::Divergence);
        const f32 fracture = texel.Get(terrain::BakedTectonicLayer::FractureDensity);
        inRange &= fracture >= -0.001F && fracture <= 1.001F;
        if (fracture > 0.25F) ++fractured;
        if (std::max({c, t, e}) < 0.02F) { ++far; farFractured += fracture > 0.1F ? 1U : 0U; }
    }
    std::cout << "fractured " << fractured
              << ", fractured far from boundaries " << farFractured << "/" << far << std::endl;
    bool ok = Check(inRange, "fracture density must stay within 0..1");

    // Boundary length by Crofton's formula: random great circles cross a curve
    // a number of times proportional to its length. Deformation adds bends,
    // jogs and splays, so the baked plate boundaries are longer.
    u32 bakedCrossings = 0, cleanCrossings = 0;
    for (u32 c = 0; c < 80U; ++c)
    {
        const math::Double3 axis = Fibonacci(c * 7U + 3U, 560U);
        const math::Double3 ref = std::abs(axis.y) < 0.9 ? math::Double3{0.0, 1.0, 0.0} : math::Double3{1.0, 0.0, 0.0};
        const math::Double3 u = math::Normalize(math::Cross(axis, ref));
        const math::Double3 v = math::Cross(axis, u);
        u32 previousBaked = 255U, previousClean = 255U;
        for (u32 k = 0; k < 1600U; ++k)
        {
            const f64 angle = 2.0 * std::numbers::pi * static_cast<f64>(k) / 1600.0;
            const math::Double3 d = u * std::cos(angle) + v * std::sin(angle);
            const u32 b = bake->Sample(d).plate;
            const u32 n = clean.GlobalFields().SampleTectonicStructure(d).plateId;
            if (previousBaked != 255U && b != previousBaked) ++bakedCrossings;
            if (previousClean != 255U && n != previousClean) ++cleanCrossings;
            previousBaked = b;
            previousClean = n;
        }
    }
    std::cout << "plate boundary crossings by 80 great circles: baked " << bakedCrossings
              << ", clean " << cleanCrossings << std::endl;
    ok &= Check(bakedCrossings > cleanCrossings * 1.1,
        "deformed plate boundaries must be longer and more complex than the clean model");
    ok &= Check(fractured > 100U, "a fracture corridor must exist around boundaries");
    ok &= Check(farFractured * 100U <= far * 3U, "fractures must stay near boundaries");
    return ok;
}

bool FaultsAreLinearNotWhorls()
{
    // Faults are stripes parallel to the boundary: at a fixed distance across
    // the boundary the pattern varies slowly along strike, and it varies fast
    // across. A generator that jitters the stripe phase with fine noise makes
    // closed whorls, where moving along strike changes the pattern as much as
    // moving across it.
    const auto planet = MakePlanet();
    const terrain::AnalyticTerrainSource source(planet, MakeDesc());
    const auto& fields = source.GlobalFields();

    f64 alongSum = 0.0;
    f64 acrossSum = 0.0;
    u32 samples = 0;
    constexpr u32 count = 20000;
    for (u32 i = 0; i < count; ++i)
    {
        const math::Double3 p = Fibonacci(i, count);
        const f64 across = 0.05 + 0.85 * static_cast<f64>((i * 7919U) % 1000U) / 1000.0;
        const f64 here = fields.FaultIntensity(p, across, 1.0);
        const math::Double3 helper = std::abs(p.y) < 0.9 ? math::Double3{0.0, 1.0, 0.0} : math::Double3{1.0, 0.0, 0.0};
        const math::Double3 e1 = math::Normalize(math::Cross(p, helper));
        const math::Double3 q = math::Normalize(p + e1 * 0.01);
        alongSum += std::abs(fields.FaultIntensity(q, across, 1.0) - here);
        acrossSum += std::abs(fields.FaultIntensity(p, across + 0.02, 1.0) - here);
        ++samples;
    }
    const f64 ratio = alongSum / std::max(acrossSum, 1.0e-12);
    std::cout << "fault pattern change along strike / across (0.01 rad vs 0.02 of the width): " << ratio
              << " over " << samples << " points" << std::endl;
    return Check(acrossSum > 0.0 && ratio < 0.6,
        "faults must vary far less along strike than across it (stripes, not whorls)");
}

bool EveryPlateOwnsTerritoryAndKeepsAnInterior()
{
    // A plate must own territory (bounded head starts, so no seed is
    // swallowed), no plate may be an isolated disc enclosed by a single other
    // plate (extra cells are merged into plates instead of unequal growth
    // rates), small plates must keep an interior (the boundary structure is
    // limited by plate size instead of painting a small plate entirely as
    // boundary), and stress must not saturate along every boundary.
    const auto planet = MakePlanet();
    bool ok = true;
    for (const u64 seed : {4242ULL, 7ULL, 99ULL})
    {
        const auto bake = terrain_bake::BakeTectonics(planet, MakeDesc(seed), {.resolution = 96});
        if (!Check(bake != nullptr, "bake must complete")) return false;
        constexpr u32 n = 60000;
        std::vector<u32> count(32, 0U), interior(32, 0U);
        std::array<std::array<u32, 32>, 32> contact{};
        u32 boundary = 0, saturated = 0;
        for (u32 i = 0; i < n; ++i)
        {
            const auto t = bake->Sample(Fibonacci(i, n));
            ++count[t.plate];
            const f32 strength = std::max({t.Get(terrain::BakedTectonicLayer::Convergence),
                t.Get(terrain::BakedTectonicLayer::Divergence), t.Get(terrain::BakedTectonicLayer::Transform)});
            if (strength > 0.1F && t.neighbour != t.plate) ++contact[t.plate][t.neighbour];
            if (strength < 0.3F) ++interior[t.plate];
            else
            {
                ++boundary;
                saturated += t.Get(terrain::BakedTectonicLayer::Stress) > 0.95F ? 1U : 0U;
            }
        }
        u32 lost = 0, hollow = 0, enclosed = 0;
        for (u32 p = 0; p < bake->PlateCount(); ++p)
        {
            if (count[p] == 0U) { ++lost; continue; }
            u32 contactTotal = 0, contactTop = 0;
            for (u32 q = 0; q < bake->PlateCount(); ++q)
            {
                contactTotal += contact[p][q];
                contactTop = std::max(contactTop, contact[p][q]);
            }
            if (contactTotal > 20U && contactTop * 10U >= contactTotal * 9U) ++enclosed;
            if (count[p] >= n / 250U && interior[p] * 4U < count[p]) ++hollow;
        }
        std::cout << "seed " << seed << ": plates without territory " << lost << ", enclosed plates " << enclosed << ", hollow plates " << hollow
                  << ", saturated stress share of boundary " << 100.0 * saturated / std::max(boundary, 1U)
                  << "%" << std::endl;
        ok &= Check(lost == 0U, "every plate must own territory");
        ok &= Check(enclosed == 0U, "no plate may be a disc enclosed by a single other plate");
        ok &= Check(hollow == 0U, "plates large enough to see must keep an interior outside the boundary structure");
        ok &= Check(saturated * 100U < boundary * 35U, "stress must not saturate along every boundary");
    }
    return ok;
}

bool BeltHeightDoesNotDependOnTheSampleFootprint()
{
    // Pages of different level sample the same place with different
    // footprints, so anything that changes with the footprint is a seam where
    // they meet. The orogenic belt comes from a smooth baked envelope, so its
    // height must not fade with the footprint (only the noise that sculpts it
    // may). It once did: belts were ~6 km high on fine pages and ~1.5-4 km on
    // coarse ones.
    const auto planet = MakePlanet();
    auto desc = MakeDesc();
    desc.global.bakedTectonics = terrain_bake::BakeTectonics(planet, desc, {.resolution = 128});
    if (!Check(desc.global.bakedTectonics != nullptr, "bake must complete")) return false;
    const terrain::AnalyticTerrainSource source(planet, desc);

    f64 worstSpread = 0.0;
    u32 points = 0;
    for (u32 i = 0; i < 200'000 && points < 24; ++i)
    {
        if (i % 11U != 0U) continue;
        const math::Double3 d = Fibonacci(i, 200'000);
        const auto g = source.GlobalFields().Sample({d, 1000.0});
        if (g.orogenEnvelope < 0.9 || g.landMask < 0.95) continue;
        ++points;
        f64 lowest = 1.0e30, highest = -1.0e30;
        for (const f64 footprint : {20'000.0, 100'000.0, 250'000.0, 375'000.0, 500'000.0, 650'000.0, 800'000.0})
        {
            const terrain::TerrainQuery q{.unitDirection = d, .footprintMeters = footprint,
                .planet = planet.id, .radialOffsetMeters = 0.0};
            const f64 elevation = source.Sample(q).elevationMeters;
            lowest = std::min(lowest, elevation);
            highest = std::max(highest, elevation);
        }
        worstSpread = std::max(worstSpread, highest - lowest);
    }
    std::cout << "belt elevation spread across footprints 20-800 km: worst " << worstSpread << " m over "
              << points << " points" << std::endl;
    return Check(points >= 12U, "belt points must exist") &&
           Check(worstSpread < 600.0, "belt height must not depend on the sample footprint (page seams)");
}

int main()
{
    bool ok = true;
    ok &= BakeIsDeterministicAndResolutionConsistent();
    ok &= BakedFieldsAreContinuousAcrossCubeEdges();
    ok &= FileRoundTripIsExactAndCorruptionIsRejected();
    ok &= BakedTerrainTracksTheProceduralTerrain();
    ok &= RecipeHashTracksOnlyBakedInputs();
    ok &= RiverBakeProducesAConsistentGraph();
    ok &= ServiceBakesRebakesAndKeepsTheRunningBakeOnFailure();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
bool CrustTypeIsIndependentOfPlatesAndHashCoversEveryTexel()
{
    const auto planet = MakePlanet();
    const auto bake = terrain_bake::BakeTectonics(planet, MakeDesc(), {.resolution = 48});
    if (!Check(bake != nullptr, "bake must complete")) return false;

    // At least one plate must carry both oceanic and continental crust.
    std::array<f32, terrain::kBakedTectonicMaxPlates> low;
    std::array<f32, terrain::kBakedTectonicMaxPlates> high;
    low.fill(1.0F);
    high.fill(0.0F);
    bool inRange = true;
    f64 thicknessLow = 0.0, thicknessHigh = 0.0;
    u32 nLow = 0, nHigh = 0;
    for (u32 i = 0; i < 6000; ++i)
    {
        const auto t = bake->Sample(Fibonacci(i, 6000U));
        const f32 f = t.Get(terrain::BakedTectonicLayer::ContinentalCrustFraction);
        inRange &= f >= -0.001F && f <= 1.001F;
        low[t.plate] = std::min(low[t.plate], f);
        high[t.plate] = std::max(high[t.plate], f);
        const f64 th = t.Get(terrain::BakedTectonicLayer::CrustThicknessKm);
        if (f < 0.25F) { thicknessLow += th; ++nLow; }
        else if (f > 0.75F) { thicknessHigh += th; ++nHigh; }
    }
    bool mixedPlate = false;
    for (u32 p = 0; p < bake->PlateCount(); ++p)
    {
        mixedPlate |= low[p] < 0.3F && high[p] > 0.7F;
    }
    bool ok = Check(inRange, "continental crust fraction must stay within 0..1");
    ok &= Check(mixedPlate, "one plate must be able to carry oceanic and continental crust");
    ok &= Check(nLow > 0U && nHigh > 0U &&
            thicknessHigh / nHigh > thicknessLow / nLow + 15.0,
        "continental crust must be much thicker than oceanic crust");

    // Changing a single texel anywhere must change the content hash.
    auto layers = std::array<std::vector<u16>, terrain::kBakedTectonicLayerCount>{};
    std::array<f32, terrain::kBakedTectonicLayerCount> minimum{}, maximum{};
    for (u32 l = 0; l < terrain::kBakedTectonicLayerCount; ++l)
    {
        const auto id = static_cast<terrain::BakedTectonicLayer>(l);
        layers[l] = bake->QuantizedLayer(id);
        minimum[l] = bake->RangeMinimum(id);
        maximum[l] = bake->RangeMaximum(id);
    }
    layers[3][layers[3].size() / 2U + 1U] ^= 1U;
    const auto altered = terrain::BakedTectonicRasters::FromQuantized(
        bake->Resolution(), bake->RecipeHash(), std::move(layers), minimum, maximum,
        bake->PlateIds(), bake->NeighbourIds(), bake->PlateContinentalFlags(),
        bake->PlateCount());
    ok &= Check(altered.ContentHash() != bake->ContentHash(),
        "a single changed texel must change the content hash");
    return ok;
}

    ok &= CrustTypeIsIndependentOfPlatesAndHashCoversEveryTexel();
    ok &= BoundariesAreNaturalisedAtBakeTime();
    ok &= FaultsAreLinearNotWhorls();
    ok &= EveryPlateOwnsTerritoryAndKeepsAnInterior();
    ok &= BeltHeightDoesNotDependOnTheSampleFootprint();
