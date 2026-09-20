#include <orbit/terrain_water/WaterService.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using namespace orbit;
using namespace orbit::terrain_water;

constexpr universe::BodyId kBody{1U, 1U};
constexpr FluidId kWater{2U, 1U};
constexpr WaterPageId kPageA{3U, 1U};
constexpr WaterPageId kPageB{3U, 2U};

void Require(const bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] FluidDefinition Water()
{
    return {
        .id = kWater,
        .name = "Water",
        .surfaceMaterial = {8U, 1U}
    };
}

[[nodiscard]] WaterPageDefinition Page(const WaterPageId id)
{
    return {
        .id = id,
        .resolution = 8U,
        .spacingMeters = 2.0,
        .fluid = kWater
    };
}

[[nodiscard]] WaterService Service(const bool ocean = false)
{
    WaterService service(kBody);
    service.RegisterFluid(Water());
    service.ConfigureOcean({
        .enabled = ocean,
        .datumHeightMeters = 1.0,
        .fluid = ocean ? kWater : FluidId{}});
    return service;
}

void AddFlatPage(WaterService& service, const WaterPageId id, const f64 bed = 0.0)
{
    const auto definition = Page(id);
    const std::vector<f64> elevations(
        static_cast<std::size_t>(definition.resolution) * definition.resolution,
        bed);
    service.CreatePage(definition, elevations);
}

void TestOceanSemantics()
{
    auto service = Service(false);
    Require(!service.IsOceanConnected(-100.0),
        "Disabled ocean classified terrain as ocean-connected.");
    service.ConfigureOcean({true, 12.0, kWater});
    Require(service.IsOceanConnected(11.9) && !service.IsOceanConnected(12.1),
        "Ocean datum did not control coastline classification.");
    AddFlatPage(service, kPageA, 10.0);
    Require(std::abs(service.FindPage(kPageA)->At(0U, 0U).depthMeters - 2.0) < 1.0e-12,
        "Ocean datum did not establish sparse still-water depth.");
}

void TestSourcesBarriersAndSnapshot()
{
    auto service = Service();
    AddFlatPage(service, kPageA);
    for (u8 placement = 0U; placement < 3U; ++placement)
    {
        service.AddSource({
            .id = {10U, static_cast<u64>(placement + 1U)},
            .page = kPageA,
            .placement = static_cast<WaterSourcePlacement>(placement),
            .x = 2U,
            .y = 2U,
            .volumeRateCubicMetersPerSecond = 1.0
        });
    }
    service.AddBarrier({
        .id = {11U, 1U},
        .page = kPageA,
        .closed = true,
        .crestHeightMeters = 5.0
    });
    const auto snapshot = service.CaptureAuthoredSnapshot();
    Require(snapshot.sources.size() == 3U && snapshot.barriers.size() == 1U,
        "Authored water definitions did not survive snapshot persistence.");

    const f64 before = service.FindPage(kPageA)->VolumeCubicMeters();
    service.Step(1.0, 4U);
    Require(service.FindPage(kPageA)->VolumeCubicMeters() > before,
        "Authored sources did not feed runtime water.");
}

void TestPhysicalAndVisualWaves()
{
    auto service = Service();
    AddFlatPage(service, kPageA);
    service.EmitPhysicalWave(kPageA, 4U, 4U, 1.0, {1.0, 0.0});
    const f64 volume = service.FindPage(kPageA)->VolumeCubicMeters();
    service.Step(1.0, 20U);
    Require(service.FindPage(kPageA)->At(5U, 4U).depthMeters > 0.0,
        "Physical wave failed to propagate into a neighboring cell.");
    Require(std::abs(service.FindPage(kPageA)->VolumeCubicMeters() - volume) < 1.0e-8,
        "Physical wave propagation did not conserve page water volume.");
    const f64 before = service.FindPage(kPageA)->VolumeCubicMeters();
    Require(service.VisualWaveHeight(kPageA, 4U, 4U, 1.0) != 0.0,
        "Visual wave layer produced no render displacement.");
    Require(service.FindPage(kPageA)->VolumeCubicMeters() == before,
        "Render-only wave altered authoritative flood volume.");
}

void TestDamBreakAndWetDry()
{
    auto service = Service();
    AddFlatPage(service, kPageA);
    service.InjectVolume(kPageA, 1U, 4U, 16.0);
    const f64 initial = service.FindPage(kPageA)->VolumeCubicMeters();
    service.Step(2.0, 80U);
    const auto* page = service.FindPage(kPageA);
    Require(std::abs(page->VolumeCubicMeters() - initial) < 1.0e-8,
        "Dam-break reference lost water mass.");
    Require(page->At(2U, 4U).depthMeters > 0.0 && page->At(1U, 4U).depthMeters < 4.0,
        "Wet/dry front did not advance and retreat stably.");
}

void TestSleepingAndHaloWake()
{
    auto service = Service();
    AddFlatPage(service, kPageA);
    AddFlatPage(service, kPageB);
    service.ConnectPages(kPageA, WaterPageSide::East, kPageB);
    service.SetSleeping(kPageA, true);
    service.SetSleeping(kPageB, true);
    service.NotifyCameraMoved(kPageA);
    Require(service.Counters().activeWaterPages == 0U,
        "Camera movement woke a sleeping physical water page.");
    service.InjectVolume(kPageA, 7U, 3U, 8.0);
    service.Step(1.0, 20U);
    Require(service.FindPage(kPageB)->At(0U, 3U).depthMeters > 0.0 &&
            service.Counters().activeWaterPages == 2U,
        "Water halo flux did not cross the page edge and wake its neighbor.");
}

void TestHullMaskAndFlooding()
{
    auto service = Service();
    AddFlatPage(service, kPageA);
    service.InjectVolume(kPageA, 3U, 3U, 10.0);
    const f64 beforeMask = service.FindPage(kPageA)->VolumeCubicMeters();
    constexpr HullMaskId mask{20U, 1U};
    service.AddExclusion({
        .owner = {21U, 1U},
        .hullMask = mask,
        .sealedInterior = true,
        .displacedVolumeCubicMeters = 3.0
    });
    Require(service.MasksRenderedWater(mask) &&
            service.FindPage(kPageA)->VolumeCubicMeters() == beforeMask,
        "Hull render masking deleted physical water mass.");

    constexpr WaterDomainId domain{22U, 1U};
    service.AddInteriorDomain({
        .id = domain,
        .capacityCubicMeters = 20.0
    });
    service.SetBreach(domain, true);
    service.ExchangeInterior(domain, kPageA, 3U, 3U, 4.0);
    const auto snapshot = service.CaptureAuthoredSnapshot();
    Require(snapshot.interiors.size() == 1U &&
            std::abs(snapshot.interiors.front().waterVolumeCubicMeters - 4.0) < 1.0e-12 &&
            std::abs(service.FindPage(kPageA)->VolumeCubicMeters() + 4.0 - beforeMask) < 1.0e-8,
        "Breach exchange failed to conserve exterior/interior water mass.");
}

void TestHullAndPropellerForcing()
{
    auto service = Service();
    AddFlatPage(service, kPageA);
    service.InjectVolume(kPageA, 4U, 4U, 8.0);
    const auto before = service.FindPage(kPageA)->At(4U, 4U);
    service.DisplaceHull(kPageA, 4U, 4U, 2.0, {40.0, 0.0});
    const auto displaced = service.FindPage(kPageA)->At(4U, 4U);
    Require(displaced.depthMeters > before.depthMeters &&
            displaced.momentumXSquareMetersPerSecond < before.momentumXSquareMetersPerSecond,
        "Moving hull did not create a physical displacement disturbance.");

    const auto forward = service.ApplyPropeller({
        .id = {30U, 1U},
        .page = kPageA,
        .x = 4U,
        .y = 4U,
        .direction = {1.0, 0.0},
        .radiusMeters = 1.0,
        .linearImpulseNewtonSeconds = 100.0,
        .angularImpulseNewtonMeterSeconds = 25.0,
        .pressureImpulse = 100.0,
        .turbulence = 0.2,
        .targetFluid = kWater
    });
    Require(std::abs(forward.bodyImpulseNewtonSeconds.x +
                     forward.waterImpulseNewtonSeconds.x) < 1.0e-12 &&
            forward.waterAngularImpulseNewtonMeterSeconds < 0.0,
        "Propeller reaction momentum or swirl direction is not conservative.");

    const auto reverse = service.ApplyPropeller({
        .id = {30U, 2U},
        .page = kPageA,
        .x = 4U,
        .y = 4U,
        .direction = {1.0, 0.0},
        .radiusMeters = 1.0,
        .linearImpulseNewtonSeconds = -100.0,
        .angularImpulseNewtonMeterSeconds = -25.0,
        .targetFluid = kWater
    });
    Require(reverse.waterImpulseNewtonSeconds.x ==
                -forward.waterImpulseNewtonSeconds.x &&
            reverse.waterAngularImpulseNewtonMeterSeconds > 0.0,
        "Reverse thrust did not reverse axial wash and swirl.");
}

void TestFluidRenderPhysicsSeparation()
{
    auto service = Service();
    AddFlatPage(service, kPageA);
    service.InjectVolume(kPageA, 1U, 1U, 4.0);
    const f64 volume = service.FindPage(kPageA)->VolumeCubicMeters();
    auto fluid = Water();
    fluid.surfaceMaterial = {99U, 99U};
    service.RegisterFluid(fluid);
    Require(service.FindFluid(kWater)->densityKgPerCubicMeter == 997.0 &&
            service.FindPage(kPageA)->VolumeCubicMeters() == volume,
        "Custom fluid surface shader changed physical density or volume.");

    auto invalid = Water();
    invalid.dynamicViscosityPascalSeconds = 10.0;
    Require(!invalid.IsValid(),
        "Water-like solver silently accepted an unvalidated high-viscosity fluid.");
    invalid.solverRegime = FluidSolverRegime::ExternallyValidated;
    Require(invalid.IsValid(),
        "Explicit externally validated fluid regime was rejected.");
}
} // namespace

int main()
{
    try
    {
        TestOceanSemantics();
        TestSourcesBarriersAndSnapshot();
        TestPhysicalAndVisualWaves();
        TestDamBreakAndWetDry();
        TestSleepingAndHaloWake();
        TestHullMaskAndFlooding();
        TestHullAndPropellerForcing();
        TestFluidRenderPhysicsSeparation();
        std::cout << "Orbit WaterService: 14/14 V0.0.4 acceptance behaviors passed.\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Orbit WaterService failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
