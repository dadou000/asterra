#include <orbit/terrain_view/ClipmapPlanner.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using namespace orbit;
using terrain_view::ClipmapConfig;
using terrain_view::ClipmapPlan;
using terrain_view::ClipmapPlanner;
using terrain_view::ClipmapPlannerConfig;
using terrain_view::ClipmapPlanView;

int gFailures = 0;

void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << "\n";
        ++gFailures;
    }
}

constexpr double kRadius = 6'371'000.0;

bool Near(const double a, const double b)
{
    return std::abs(a - b) <= 1.0e-6 * std::max(std::abs(a), std::abs(b));
}

// 20 ladder levels from 1 m, 65 samples across: reaches ~16,800 km.
ClipmapConfig Ladder()
{
    return {
        .levelCount = 20,
        .gridResolution = 65,
        .baseSpacingMeters = 1.0,
        .levelScale = 2.0,
        .overlapCells = 6};
}

// The camera sits on +X at `altitude`, looking along `pitchDegrees` below the
// horizon (90 = straight down, 0 = at the horizon, negative = at the sky).
ClipmapPlanView View(const double altitude, const double pitchDegrees, const double elevation = 0.0)
{
    constexpr double kPi = 3.14159265358979323846;
    ClipmapPlanView view;
    view.position = {kRadius + altitude + elevation, 0.0, 0.0};
    const double pitch = pitchDegrees * kPi / 180.0;
    // Tangent plane at +X is spanned by Y (up for the camera roll) and Z.
    view.forward = {-std::sin(pitch), 0.0, std::cos(pitch)};
    view.up = {std::cos(pitch), 0.0, std::sin(pitch)};
    view.verticalFovRadians = 70.0 * kPi / 180.0;
    view.viewportWidthPixels = 1920;
    view.viewportHeightPixels = 1080;
    view.planetRadiusMeters = kRadius;
    view.groundElevationMeters = elevation;
    return view;
}

ClipmapPlan PlanOnce(const ClipmapPlanView& view, ClipmapPlannerConfig config = {})
{
    ClipmapPlanner planner(config);
    return planner.Plan(Ladder(), view);
}

void TestNadirNeedsFewLevels()
{
    const ClipmapPlan low = PlanOnce(View(1000.0, 90.0));
    Check(low.dynamic, "enabled planner is dynamic");
    Check(low.groundInView, "looking down sees ground");
    Check(low.firstLevel >= 1U && low.firstLevel <= 2U,
          "1 km nadir: levels finer than ~2 m are dropped");
    Check(low.lastLevel >= 5U && low.lastLevel <= 7U,
          "1 km nadir: coverage stops near 2 km, not the full ladder");
    Check(low.ActiveLevels() <= 7U, "1 km nadir needs at most seven levels of twenty");

    const ClipmapPlan high = PlanOnce(View(10'000.0, 90.0));
    Check(high.firstLevel >= low.firstLevel + 2U, "10 km nadir drops two more octaves of fine levels");
    Check(high.lastLevel > low.lastLevel, "higher altitude sees more ground");
}

void TestHorizonNeedsCoverage()
{
    const ClipmapPlan horizon = PlanOnce(View(1000.0, 0.0));
    // Horizon arc from 1 km is ~113 km; the half extent must reach it.
    Check(horizon.coarsestHalfExtentMeters >= 113'000.0,
          "a horizon view covers the horizon arc");
    Check(horizon.lastLevel >= 12U, "a horizon view needs the coarse levels");
    const ClipmapPlan down = PlanOnce(View(1000.0, 90.0));
    Check(horizon.firstLevel <= down.firstLevel + 1U,
          "the ground below still counts in a horizon view");
}

void TestSkyKeepsTheGroundBelow()
{
    const ClipmapPlan sky = PlanOnce(View(1000.0, -45.0));
    Check(!sky.groundInView, "looking at the sky shows no ground");
    const ClipmapPlan down = PlanOnce(View(1000.0, 90.0));
    Check(sky.firstLevel == down.firstLevel,
          "a sky view still keeps the fine levels for the ground below");
    Check(sky.ActiveLevels() >= 3U, "a sky view keeps a usable range");
}

void TestElevationShiftsDemand()
{
    // 5 km above sea level but only 100 m above the ground: fine levels stay.
    const ClipmapPlan onGround = PlanOnce(View(100.0, 90.0, 4900.0));
    Check(onGround.firstLevel == 0U, "close to high ground keeps the finest level");
    const ClipmapPlan inAir = PlanOnce(View(5000.0, 90.0, 0.0));
    Check(inAir.firstLevel >= 2U, "the same sea-level altitude over sea drops fine levels");
}

void TestDisabledKeepsEverything()
{
    ClipmapPlannerConfig config;
    config.enabled = false;
    const ClipmapPlan plan = PlanOnce(View(1000.0, 90.0), config);
    Check(!plan.dynamic && plan.firstLevel == 0U && plan.lastLevel == 19U,
          "a disabled planner activates the whole ladder");
}

void TestMonotonicWithAltitude()
{
    ClipmapPlanner planner;
    ClipmapPlan previous = planner.Plan(Ladder(), View(50.0, 90.0));
    for (double altitude = 60.0; altitude < 3.0e6; altitude *= 1.07)
    {
        const ClipmapPlan plan = planner.Plan(Ladder(), View(altitude, 90.0));
        Check(plan.firstLevel >= previous.firstLevel, "finest level never gets finer as the camera rises");
        Check(plan.lastLevel >= previous.lastLevel, "coverage never shrinks as the camera rises");
        previous = plan;
    }
}

void TestHysteresis()
{
    // Wiggle the altitude +-4% around a point where the finest level changes,
    // and count how often the plan changes.
    // The boundary is where the plain (no hysteresis) planner changes level.
    ClipmapPlannerConfig plain;
    plain.hysteresisOctaves = 0.0;
    ClipmapPlanner planner(plain);
    ClipmapPlan reference = planner.Plan(Ladder(), View(100.0, 90.0));
    double boundary = 0.0;
    for (double altitude = 100.0; altitude < 100'000.0; altitude *= 1.01)
    {
        const ClipmapPlan plan = planner.Plan(Ladder(), View(altitude, 90.0));
        if (plan.firstLevel != reference.firstLevel)
        {
            boundary = altitude;
            break;
        }
    }
    Check(boundary > 0.0, "found an altitude where the finest level changes");

    ClipmapPlanner wiggler;
    static_cast<void>(wiggler.Plan(Ladder(), View(boundary, 90.0)));
    int changes = 0;
    ClipmapPlan prior = wiggler.Last();
    for (int i = 0; i < 400; ++i)
    {
        const double altitude = boundary * (1.0 + 0.04 * std::sin(i * 0.37));
        const ClipmapPlan plan = wiggler.Plan(Ladder(), View(altitude, 90.0));
        if (!(plan == prior))
        {
            ++changes;
        }
        prior = plan;
    }
    Check(changes <= 2, "hysteresis keeps a camera sitting on a boundary from flickering levels");

    // The same wiggle without hysteresis does flicker, so the test is meaningful.
    ClipmapPlannerConfig loose;
    loose.hysteresisOctaves = 0.0;
    ClipmapPlanner raw(loose);
    static_cast<void>(raw.Plan(Ladder(), View(boundary, 90.0)));
    int rawChanges = 0;
    ClipmapPlan rawPrior = raw.Last();
    for (int i = 0; i < 400; ++i)
    {
        const double altitude = boundary * (1.0 + 0.04 * std::sin(i * 0.37));
        const ClipmapPlan plan = raw.Plan(Ladder(), View(altitude, 90.0));
        if (!(plan == rawPrior))
        {
            ++rawChanges;
        }
        rawPrior = plan;
    }
    Check(rawChanges > changes, "without hysteresis the same motion changes the plan more often");
}

void TestCoverageInvariant()
{
    // Whatever the pose, the coarsest active level reaches the farthest visible
    // ground the planner itself measured (up to the ladder's own limit).
    ClipmapPlanner planner;
    for (const double altitude : {10.0, 300.0, 5000.0, 80'000.0, 1.5e6})
    {
        for (const double pitch : {90.0, 60.0, 30.0, 5.0, -10.0})
        {
            const ClipmapPlan plan = planner.Plan(Ladder(), View(altitude, pitch));
            const bool reaches = plan.coarsestHalfExtentMeters >= plan.visibleArcMeters ||
                                 plan.lastLevel == 19U;
            Check(reaches, "coarsest active level covers the visible arc");
            Check(plan.firstLevel <= plan.lastLevel && plan.lastLevel <= 19U,
                  "the plan stays inside the ladder");
        }
    }
}

void TestPixelsPerVertexTradesDetail()
{
    ClipmapPlannerConfig fine;
    fine.pixelsPerVertex = 1.0;
    ClipmapPlannerConfig coarse;
    coarse.pixelsPerVertex = 8.0;
    const ClipmapPlan detailed = PlanOnce(View(20'000.0, 90.0), fine);
    const ClipmapPlan relaxed = PlanOnce(View(20'000.0, 90.0), coarse);
    Check(detailed.firstLevel < relaxed.firstLevel,
          "a lower pixels-per-vertex target keeps finer levels active");
}
ClipmapConfig CoarseDenseLadder()
{
    ClipmapConfig config = Ladder();
    config.coarseGridResolution = 257;
    config.coarseMinSpacingMeters = 64.0;
    return config;
}

void TestPerLevelGridResolution()
{
    const ClipmapConfig config = CoarseDenseLadder();
    Check(terrain_view::ClipmapLevelGridResolution(config, 5) == 65U,
          "a 32 m level keeps the fine grid");
    Check(terrain_view::ClipmapLevelGridResolution(config, 6) == 257U,
          "a 64 m level uses the coarse grid");

    const auto layout = terrain_view::BuildClipmapLayout(
        config, {.meters = {kRadius + 1000.0, 0.0, 0.0}});
    Check(layout.levels.size() == 20U, "layout keeps every level");
    bool holesMatch = true;
    for (std::size_t i = 1U; i < layout.levels.size(); ++i)
    {
        holesMatch = holesMatch &&
            layout.levels[i].innerHoleHalfExtentMeters ==
                layout.levels[i - 1U].outerHalfExtentMeters;
    }
    Check(holesMatch, "each level's hole is the finer level's outer extent");
    Check(layout.levels[5].gridResolution == 65U &&
              layout.levels[6].gridResolution == 257U,
          "the layout switches resolution at the threshold");
    // The coarse level's cells are twice its half extent over 4x the cells.
    Check(Near(layout.levels[6].outerHalfExtentMeters, 128.0 * 64.0),
          "a coarse level's extent follows its own half-cell count");
    Check(layout.levels[6].morphStartHalfExtentMeters >
              layout.levels[6].innerHoleHalfExtentMeters,
          "the coarse morph band starts outside the hole");
    Check(Near(terrain_view::ClipmapOuterHalfExtentMeters(config),
               terrain_view::ClipmapLevelHalfExtentMeters(config, 19)),
          "the ladder's reach is its coarsest level's half extent");
}

void TestDenseCoarseLevelsNeedFewerLevels()
{
    const ClipmapPlanView view = View(1000.0, 0.0);
    ClipmapPlanner plain;
    ClipmapPlanner dense;
    const ClipmapPlan a = plain.Plan(Ladder(), view);
    const ClipmapPlan b = dense.Plan(CoarseDenseLadder(), view);
    Check(b.coarsestHalfExtentMeters >= b.visibleArcMeters,
          "dense coarse levels still reach the visible ground");
    Check(b.lastLevel < a.lastLevel,
          "a denser coarse grid reaches the horizon with fewer levels");
}
} // namespace

// EXPERIMENT: distance-banded clipmap.
ClipmapConfig Banded()
{
    ClipmapConfig config{};
    config.gridResolution = 513;
    config.bandCount = 5;
    config.bandEdgesMeters[0] = 100.0;
    config.bandEdgesMeters[1] = 500.0;
    config.bandEdgesMeters[2] = 2000.0;
    config.bandEdgesMeters[3] = 10000.0;
    config.bandEdgesMeters[4] = 40000.0;
    return config;
}

void TestBandedLayout()
{
    const ClipmapConfig config = Banded();
    const orbit::world::WorldPosition observer{.meters = {kRadius + 10.0, 0.0, 0.0}};
    const auto layout = terrain_view::BuildClipmapLayout(config, observer);
    Check(layout.levels.size() == 5U, "one level per band");
    for (u32 level = 0; level < 5U; ++level)
    {
        const auto band = terrain_view::ClipmapLevelBand(config, level);
        Check(band.innerMeters == (level == 0U ? 0.0 : config.bandEdgesMeters[level - 1U]),
              "band inner edge is the previous edge");
        Check(band.outerMeters == config.bandEdgesMeters[level], "band outer edge");
        // The window covers the outer edge plus the margin, and the cross-fade zone.
        Check(layout.levels[level].outerHalfExtentMeters >=
                  band.outerMeters * (1.0 + config.bandZoneFraction),
              "window covers the band and its cross-fade zone");
        Check(layout.levels[level].innerHoleHalfExtentMeters == 0.0, "banded levels have no hole");
        Check(layout.levels[level].gridResolution == 513U, "banded grid resolution");
        if (level > 0U)
        {
            Check(layout.levels[level].sampleSpacingMeters >
                      layout.levels[level - 1U].sampleSpacingMeters,
                  "spacing grows with the band");
        }
    }
    bool threw = false;
    ClipmapConfig bad = config;
    bad.bandEdgesMeters[2] = 300.0;
    try { static_cast<void>(terrain_view::BuildClipmapLayout(bad, observer)); }
    catch (const std::invalid_argument&) { threw = true; }
    Check(threw, "non-increasing band edges are rejected");
}

void TestBandedPlannerFollowsDistance()
{
    ClipmapPlanner planner{ClipmapPlannerConfig{}};
    const ClipmapConfig config = Banded();
    // On the ground, looking down: only the nearest bands.
    const ClipmapPlan low = planner.Plan(config, View(20.0, 90.0));
    Check(low.firstLevel == 0U, "near the ground band 0 is wanted");
    // 3 km up looking down: nothing is nearer than ~3 km, so bands 0..2 are not
    // needed (band 2 reaches 2000 m x 1.15 x 1.1 = 2530 m < 3000 m).
    const ClipmapPlan high = planner.Plan(config, View(3000.0, 90.0));
    Check(high.firstLevel >= 3U, "bands nearer than the ground are dropped");
    Check(high.lastLevel >= high.firstLevel, "a usable range");
    // Looking at the horizon from 3 km sees ground up to ~200 km away.
    const ClipmapPlan horizon = planner.Plan(config, View(3000.0, 0.0));
    Check(horizon.farthestDistanceMeters > 150000.0, "the horizon is far");
    Check(horizon.lastLevel >= 4U, "far bands are wanted toward the horizon");
    // Monotone: climbing never brings back a nearer band.
    u32 previousFirst = 0U;
    for (const double altitude : {10.0, 100.0, 400.0, 1500.0, 6000.0, 30000.0})
    {
        const ClipmapPlan plan = planner.Plan(config, View(altitude, 90.0));
        Check(plan.firstLevel >= previousFirst, "first band never decreases with altitude");
        previousFirst = plan.firstLevel;
    }
}

int main()
{
    try
    {
        TestNadirNeedsFewLevels();
        TestHorizonNeedsCoverage();
        TestSkyKeepsTheGroundBelow();
        TestElevationShiftsDemand();
        TestDisabledKeepsEverything();
        TestMonotonicWithAltitude();
        TestHysteresis();
        TestCoverageInvariant();
        TestPixelsPerVertexTradesDetail();
        TestPerLevelGridResolution();
        TestDenseCoarseLevelsNeedFewerLevels();
        TestBandedLayout();
        TestBandedPlannerFollowsDistance();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: unexpected exception: " << exception.what() << "\n";
        return 1;
    }
    if (gFailures == 0)
    {
        std::cout << "Orbit clipmap planner tests passed.\n";
    }
    return gFailures == 0 ? 0 : 1;
}
