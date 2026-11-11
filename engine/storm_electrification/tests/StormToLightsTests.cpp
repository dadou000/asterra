// Integration: a flash simulated by the storm solver, handed to the transient flash registry, must come out as lights
// that carry its optical energy and sit where its channel is.
#include <orbit/lighting/TransientFlashRegistry.hpp>
#include <orbit/storm_electrification/Electrification.hpp>
#include <orbit/storm_electrification/LeaderSimulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

using namespace orbit;
using namespace orbit::storm_electrification;

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

StormFields MakeStorm(const ChargeGrid& g, double peakUpdraft)
{
    StormFields f;
    const std::size_t n = g.CellCount();
    f.temperatureK.resize(n); f.airDensity.resize(n); f.updraft.resize(n);
    f.graupel.resize(n); f.ice.resize(n); f.liquid.resize(n);
    const double cx = g.Nx() * 0.5 * g.Spacing(), cy = g.Ny() * 0.5 * g.Spacing();
    for (std::uint32_t k = 0; k < g.Nz(); ++k)
        for (std::uint32_t j = 0; j < g.Ny(); ++j)
            for (std::uint32_t i = 0; i < g.Nx(); ++i)
            {
                const std::size_t c = g.Index(i, j, k);
                const double z = g.Height(k);
                const double r = std::hypot((i + 0.5) * g.Spacing() - cx, (j + 0.5) * g.Spacing() - cy);
                const double column = std::exp(-r * r / (2.0 * 4000.0 * 4000.0));
                const double vertical = std::sin(std::numbers::pi * std::clamp((z - 500.0) / 12000.0, 0.0, 1.0));
                const double temperature = 300.0 - 6.5e-3 * z;
                f.temperatureK[c] = static_cast<float>(temperature);
                f.airDensity[c] = static_cast<float>(1.2 * std::exp(-z / 8400.0));
                f.updraft[c] = static_cast<float>(peakUpdraft * column * vertical);
                const bool cold = temperature < 268.0, veryCold = temperature < 253.0;
                const double strength = peakUpdraft / 25.0;
                f.graupel[c] = static_cast<float>(cold ? 2.0e-3 * column * strength : 0.0);
                f.ice[c] = static_cast<float>((cold ? 1.0e-3 : 0.0) * std::exp(-r * r / (2.0 * 6000.0 * 6000.0)) * (veryCold ? 2.0 : 1.0) * strength);
                const double liquidFraction = std::clamp((temperature - 253.0) / (273.15 - 253.0), 0.0, 1.0);
                f.liquid[c] = static_cast<float>(1.5e-3 * column * std::min(1.0, strength) * liquidFraction);
            }
    return f;
}
} // namespace

int main()
{
    ChargeGrid grid(48, 48, 32, 500.0);
    const StormFields storm = MakeStorm(grid, 25.0);
    const ElectrificationParams electrification;
    LeaderParams leader;
    for (double t = 0.0; t < 1200.0 && MaxBreakdownRatio(grid, leader) < 1.0; t += 5.0)
    {
        StepElectrification(grid, storm, electrification, 5.0);
        grid.SolvePotential();
    }
    const FlashResult flash = TryFlash(grid, leader, 0.0, 0);
    Check(flash.occurred, "the charged storm flashes");
    const double total = flash.graph.EnergyInWindow(-1.0, 10.0);
    Check(total > 0.0, "the simulated flash radiates");

    lighting::TransientFlashRegistry registry;
    const math::Double3 origin{20000.0, -5000.0, 0.0};
    registry.Add(flash.graph, origin, 100.0, 5.0);

    lighting::CameraPlacement camera;
    lighting::TransientLightParams params;
    double lumenSeconds = 0.0;
    double minX = 1e300, maxX = -1e300;
    for (double t = 99.5; t < 105.5; t += params.exposureSeconds)
    {
        const auto lights = registry.LightsForWindow(t, camera, params);
        Check(lights.size() <= params.maxLights, "budget respected");
        for (const auto& l : lights)
        {
            lumenSeconds += l.luminousFluxLumens * params.exposureSeconds;
            minX = std::min(minX, l.positionInFrameMeters.x);
            maxX = std::max(maxX, l.positionInFrameMeters.x);
        }
    }
    const double expected = total * params.luminousEfficacyOfRadiation;
    std::printf("flash %.3g J -> %.4g lumen-s (expected %.4g), lights span x %.0f..%.0f\n", total, lumenSeconds, expected, minX, maxX);
    Check(std::abs(lumenSeconds - expected) < 2.0e-3 * expected, "the lights carry the simulated flash's energy");

    double gx0 = 1e300, gx1 = -1e300;
    for (const auto& s : flash.graph.Segments())
    {
        gx0 = std::min({gx0, s.from[0], s.to[0]});
        gx1 = std::max({gx1, s.from[0], s.to[0]});
    }
    Check(minX >= origin.x + gx0 - 1.0 && maxX <= origin.x + gx1 + 1.0, "lights stay inside the channel's extent");
    Check(registry.LightsForWindow(200.0, camera, params).empty() && registry.Empty(), "the flash is removed after its lifetime");
    std::printf(failures == 0 ? "StormToLights tests passed\n" : "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
