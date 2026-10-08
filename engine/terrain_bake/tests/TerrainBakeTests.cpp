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

bool BakeReproducesThePlateModel()
{
    const auto planet = MakePlanet();
    const auto desc = MakeDesc();
    const auto bake = terrain_bake::BakeTectonics(planet, desc, {.resolution = 128});
    bool ok = Check(bake != nullptr, "bake must complete");
    if (!ok) return false;

    const terrain::AnalyticTerrainSource model(planet, desc);
    f64 worstConvergence = 0.0;
    f64 worstBias = 0.0;
    f64 worstThickness = 0.0;
    f64 worstAge = 0.0;
    u32 plateMismatches = 0;
    constexpr u32 count = 20000;
    for (u32 i = 0; i < count; ++i)
    {
        const auto d = Fibonacci(i, count);
        const auto texel = bake->Sample(d);
        const auto truth = model.GlobalFields().EvaluateTectonicTexel(d);
        worstConvergence = std::max(worstConvergence, std::abs(
            static_cast<f64>(texel.Get(terrain::BakedTectonicLayer::Convergence) -
                             truth.Get(terrain::BakedTectonicLayer::Convergence))));
        worstBias = std::max(worstBias, std::abs(
            static_cast<f64>(texel.Get(terrain::BakedTectonicLayer::PlateBiasMeters) -
                             truth.Get(terrain::BakedTectonicLayer::PlateBiasMeters))));
        worstThickness = std::max(worstThickness, std::abs(
            static_cast<f64>(texel.Get(terrain::BakedTectonicLayer::CrustThicknessKm) -
                             truth.Get(terrain::BakedTectonicLayer::CrustThicknessKm))));
        worstAge = std::max(worstAge, std::abs(
            static_cast<f64>(texel.Get(terrain::BakedTectonicLayer::CrustAge) -
                             truth.Get(terrain::BakedTectonicLayer::CrustAge))));
        if (texel.plate != truth.plate) ++plateMismatches;
    }
    std::cout << "bake fidelity @128: convergence " << worstConvergence
              << ", bias " << worstBias << " m, thickness " << worstThickness
              << " km, age " << worstAge << ", plate id mismatches "
              << plateMismatches << " of " << count << '\n';
    ok &= Check(worstConvergence < 0.12, "baked convergence must track the plate model");
    ok &= Check(worstBias < 600.0, "baked plate bias must track the plate model");
    ok &= Check(worstThickness < 6.0, "baked crust thickness must track the plate model");
    ok &= Check(worstAge < 0.12, "baked crust age must track the plate model");
    ok &= Check(plateMismatches < count / 60U, "plate identity may differ only at boundaries");
    return ok;
}

bool BakedFieldsAreAccurateAtCubeEdges()
{
    // Texels near a cube edge interpolate through the gutter, which is
    // evaluated on the neighbouring face's geometry. Their error against the
    // plate model must be no worse than anywhere else on the planet.
    const auto planet = MakePlanet();
    const auto desc = MakeDesc();
    const auto bake = terrain_bake::BakeTectonics(planet, desc, {.resolution = 64});
    if (!Check(bake != nullptr, "bake must complete")) return false;
    const terrain::AnalyticTerrainSource model(planet, desc);

    const auto worstError = [&](const auto& pointAt)
    {
        f64 worst = 0.0;
        u32 worstLayer = 0;
        for (u32 i = 0; i < 6000; ++i)
        {
            const math::Double3 d = pointAt(static_cast<f64>(i) / 6000.0, i);
            const auto texel = bake->Sample(d);
            const auto truth = model.GlobalFields().EvaluateTectonicTexel(d);
            for (u32 layer = 0; layer < terrain::kBakedTectonicLayerCount; ++layer)
            {
                const auto id = static_cast<terrain::BakedTectonicLayer>(layer);
                const f64 range = static_cast<f64>(bake->RangeMaximum(id) - bake->RangeMinimum(id));
                const f64 error = std::abs(static_cast<f64>(texel.values[layer] - truth.values[layer])) / range;
                if (error > worst)
                {
                    worst = error;
                    worstLayer = layer;
                }
            }
        }
        return std::pair<f64, u32>{worst, worstLayer};
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
    const auto anywherePoints = [](const f64, const u32 i)
    {
        return Fibonacci(i, 6000U);
    };

    const auto edge = worstError(edgePoints);
    const auto anywhere = worstError(anywherePoints);
    std::cout << "worst normalized error vs plate model: near cube edges " << edge.first
              << " (layer " << edge.second << "), anywhere " << anywhere.first
              << " (layer " << anywhere.second << ")\n";
    return Check(edge.first <= anywhere.first * 1.5 + 0.005,
        "baked layers must be as accurate across cube-face edges as inside a face");
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
              << " m, worst " << worst << " m\n";
    // The bake carries structure the unbaked plate model does not: crust-type
    // driven geography, orogenic belts that exist at zero noise, and the
    // structural elevation (trench, arc, ridge, rift). So the two agree in
    // scale, not exactly, and the baked difference has the right sign where
    // the structure says so.
    ok &= Check(sumAbs / count < 1200.0, "baked terrain must stay at the procedural terrain's scale");
    ok &= Check(worst < 9000.0, "baked terrain must not deviate wildly anywhere");

    f64 trenchSum = 0.0, arcSum = 0.0;
    u32 trenchCount = 0, arcCount = 0;
    for (u32 i = 0; i < 60'000; ++i)
    {
        const math::Double3 direction = Fibonacci(i, 60'000);
        const auto structure = baked.GlobalFields().SampleTectonicStructure(direction);
        const bool trench = structure.subductionTrench > 0.6;
        const bool arc = structure.volcanicArc > 0.6;
        if (!trench && !arc) continue;
        const terrain::TerrainQuery query{
            .unitDirection = direction, .footprintMeters = 20'000.0,
            .planet = planet.id, .radialOffsetMeters = 0.0};
        const f64 delta = baked.Sample(query).elevationMeters - procedural.Sample(query).elevationMeters;
        if (trench) { trenchSum += delta; ++trenchCount; }
        else { arcSum += delta; ++arcCount; }
    }
    std::cout << "baked - procedural: trench " << (trenchCount ? trenchSum / trenchCount : 0.0)
              << " m (" << trenchCount << "), arc " << (arcCount ? arcSum / arcCount : 0.0)
              << " m (" << arcCount << ")\n";
    ok &= Check(trenchCount > 0U && trenchSum / trenchCount < -800.0,
        "trenches must reach the baked terrain as depressions");
    ok &= Check(arcCount > 0U && arcSum / arcCount > 0.0,
        "volcanic arcs must reach the baked terrain as uplift");

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

int main()
{
    bool ok = true;
    ok &= BakeReproducesThePlateModel();
    ok &= BakedFieldsAreAccurateAtCubeEdges();
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
