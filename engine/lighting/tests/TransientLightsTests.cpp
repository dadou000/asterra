#include <orbit/lighting/TransientLights.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace orbit;
using namespace orbit::lighting;
using optical_transport::DischargeGraph;
using optical_transport::FlashKind;
using optical_transport::Segment;

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
bool Near(double a, double b, double rel) { return std::abs(a - b) <= rel * std::max(std::abs(a), std::abs(b)) + 1.0e-300; }

// A cloud-to-ground-like flash: `count` leader segments spread over 10 km x 10 km x 8 km, each with a faint pulse as it
// forms, then a 30 microsecond return stroke of 300 kJ spread over the path, starting at `strokeTime`.
DischargeGraph MakeFlash(int count, double strokeTime)
{
    DischargeGraph graph(FlashKind::CloudToGround);
    for (int i = 0; i < count; ++i)
    {
        Segment s;
        s.from[0] = 1000.0 + (i % 40) * 250.0; s.from[1] = 2000.0 + (i / 40 % 40) * 150.0; s.from[2] = 500.0 + (i / 1600) * 300.0;
        s.to[0] = s.from[0] + 100.0; s.to[1] = s.from[1] + 60.0; s.to[2] = s.from[2];
        s.formedAt = i * 1.0e-5;
        s.pulses.push_back({s.formedAt, 1.0e-5, 0.5});
        graph.AddSegment(s);
    }
    for (int i = 0; i < count; ++i)
    {
        graph.Reactivate(i, {strokeTime + i * 1.0e-8, 3.0e-5, 3.0e5 / count});
    }
    return graph;
}
} // namespace

int main()
{
    const DischargeGraph flash = MakeFlash(400, 0.020);
    const double total = flash.EnergyInWindow(0.0, 1.0);

    TransientLightParams params;
    params.exposureSeconds = 1.0 / 60.0;
    const math::Double3 origin{100000.0, 0.0, -50000.0};

    // The stroke (t = 20 ms) falls in the second 16.7 ms window.
    const double window = params.exposureSeconds;
    const auto first = TransientLightsFromFlash(flash, origin, 0.0, params, 1000);
    const auto second = TransientLightsFromFlash(flash, origin, window, params, 2000);
    const auto third = TransientLightsFromFlash(flash, origin, 2.0 * window, params, 3000);
    const auto energyOf = [&](const std::vector<LocalLight>& lights) {
        double e = 0.0;
        for (const auto& l : lights) e += l.luminousFluxLumens / params.luminousEfficacyOfRadiation * params.exposureSeconds;
        return e;
    };

    Check(!second.empty() && second.size() <= params.maxLights, "lights are bounded by the budget");
    Check(Near(energyOf(second), flash.EnergyInWindow(window, 2.0 * window), 1.0e-5), "lights carry exactly the window's energy");
    Check(Near(energyOf(first) + energyOf(second) + energyOf(third), total, 1.0e-5), "consecutive windows partition the flash energy");
    Check(energyOf(second) > 0.9 * 3.0e5, "a 30 microsecond stroke inside a 16 ms window deposits its whole energy");
    Check(energyOf(first) < 0.01 * energyOf(second) && energyOf(third) < 0.01 * energyOf(second), "the stroke window is far brighter than its neighbours");

    // The same stroke integrated over a longer exposure is dimmer per unit time but equal in energy.
    TransientLightParams longer = params;
    longer.exposureSeconds = 0.1;
    const auto wide = TransientLightsFromFlash(flash, origin, 0.0, longer, 0);
    double wideFlux = 0.0, narrowFlux = 0.0;
    for (const auto& l : wide) wideFlux += l.luminousFluxLumens;
    for (const auto& l : second) narrowFlux += l.luminousFluxLumens;
    Check(wideFlux < narrowFlux, "a longer exposure averages the stroke down to a lower flux");
    double wideEnergy = 0.0;
    for (const auto& l : wide) wideEnergy += l.luminousFluxLumens / longer.luminousEfficacyOfRadiation * longer.exposureSeconds;
    Check(Near(wideEnergy, flash.EnergyInWindow(0.0, 0.1), 1.0e-5), "energy is preserved at any exposure length");

    // Ordering, placement, ranges.
    Check(std::is_sorted(second.begin(), second.end(), [](const auto& a, const auto& b) { return a.luminousFluxLumens > b.luminousFluxLumens; }),
          "lights are ordered by decreasing flux");
    bool placed = true, ranged = true, ids = true;
    for (std::size_t i = 0; i < second.size(); ++i)
    {
        placed = placed && second[i].positionInFrameMeters.x >= origin.x + 1000.0 && second[i].positionInFrameMeters.z <= origin.z + 2000.0;
        ranged = ranged && second[i].rangeMeters >= params.minimumRangeMeters && second[i].rangeMeters <= params.maximumRangeMeters;
        ids = ids && second[i].stableId == 2000 + i;
    }
    Check(placed, "the graph is placed at the requested origin");
    Check(ranged, "ranges respect the configured limits");
    Check(ids, "stable ids are consecutive from the base");
    Check(second.front().rangeMeters > 10000.0, "a stroke lights things tens of kilometres away");

    // Budget independent of flash complexity.
    const DischargeGraph huge = MakeFlash(8000, 0.020);
    TransientLightParams tight = params;
    tight.maxLights = 6;
    const auto small = TransientLightsFromFlash(huge, origin, window, tight, 0);
    double smallEnergy = 0.0;
    for (const auto& l : small) smallEnergy += l.luminousFluxLumens / tight.luminousEfficacyOfRadiation * tight.exposureSeconds;
    Check(small.size() <= 6 && Near(smallEnergy, huge.EnergyInWindow(window, 2.0 * window), 1.0e-5), "an 8000-segment flash still yields at most 6 lights with all its energy");

    // Nothing to emit, nothing to draw.
    Check(TransientLightsFromFlash(flash, origin, 5.0, params, 0).empty(), "a window after the flash has no lights");
    TransientLightParams none = params;
    none.maxLights = 0;
    Check(TransientLightsFromFlash(flash, origin, window, none, 0).empty(), "a zero budget yields no lights");

    // They pass through the existing tiled-light path without losing flux, even with a per-tile cap of 2.
    LightingView view;
    view.forward = {0.0F, 0.0F, 1.0F};
    view.up = {0.0F, 1.0F, 0.0F};
    view.verticalFovRadians = 1.0F;
    std::vector<LocalLight> crowd = second;
    for (auto& l : crowd) l.positionInFrameMeters = {l.positionInFrameMeters.x - origin.x - 5000.0, l.positionInFrameMeters.y - 500.0, l.positionInFrameMeters.z - origin.z + 3000.0};
    const auto grid = BuildTiledLightGrid(crowd, view, 64U, 64U, {.tileSizePixels = 16U, .maximumLightsPerTile = 2U});
    double listed = 0.0;
    for (u32 i = grid.offsets[0]; i < grid.offsets[1]; ++i) listed += grid.lights[grid.lightIndices[i]].luminousFluxLumens;
    double wanted = 0.0;
    for (const auto& l : crowd) wanted += l.luminousFluxLumens;
    Check(grid.proxyLights > 0 || crowd.size() <= 2U, "overflow beyond the tile cap is carried by a proxy");
    Check(Near(listed, wanted, 1.0e-4), "tile flux equals the flash flux despite a tiny per-tile cap");

    if (failures == 0) std::printf("TransientLights: all tests passed\n");
    return failures;
}
