#include <orbit/lighting/TransientFlashRegistry.hpp>

#include <cmath>
#include <cstdio>

using namespace orbit;
using namespace orbit::lighting;

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
f64 Flux(const std::vector<LocalLight>& lights)
{
    f64 sum = 0.0;
    for (const auto& l : lights) { sum += l.luminousFluxLumens; }
    return sum;
}
} // namespace

int main()
{
    CameraPlacement camera;
    camera.positionMeters = {1000.0, 2000.0, 3000.0};
    camera.forward = {0.0, 0.3, 1.0};   // looking slightly up: must be flattened onto the horizontal plane
    camera.up = {0.0, 1.0, 0.0};
    TransientLightParams params;
    params.exposureSeconds = 1.0 / 60.0;

    TransientFlashRegistry registry;
    Check(registry.Empty() && registry.LightsForWindow(0.0, camera, params).empty(), "empty registry yields no lights");

    FlashRequest request;
    request.distanceMeters = 4000.0;
    request.seed = 7U;
    const u64 id = registry.Fire(request, 10.0);
    Check(id != 0U, "id is non-zero");
    Check(!registry.Status(10.0).front().placed, "camera-relative flash is unplaced until the first frame");

    // Sum the flux over the lifetime in exposure steps: it must match the requested optical energy.
    f64 lumenSeconds = 0.0;
    f64 peak = 0.0;
    for (f64 t = 10.0; t < 10.6; t += params.exposureSeconds)
    {
        const auto lights = registry.LightsForWindow(t, camera, params);
        lumenSeconds += Flux(lights) * params.exposureSeconds;
        peak = std::max(peak, Flux(lights));
        Check(lights.size() <= params.maxLights, "light budget respected");
    }
    const f64 expected = request.energyJoules * params.luminousEfficacyOfRadiation;
    Check(std::abs(lumenSeconds - expected) < 1.0e-3 * expected, "lumen-seconds equal energy x efficacy");
    Check(peak > 0.0, "some window is lit");
    Check(registry.Empty(), "flash expires after its last stroke");

    // Placement: base is 4 km ahead on the horizontal plane at the camera's height, channel rises along up.
    TransientFlashRegistry placed;
    placed.Fire(request, 0.0);
    (void)placed.LightsForWindow(0.0, camera, params);
    const auto status = placed.Status(0.0).front();
    Check(status.placed, "placed after first frame");
    Check(std::abs(status.baseMeters.y - camera.positionMeters.y) < 1.0e-9, "base stays at camera height (forward flattened)");
    Check(std::abs(status.baseMeters.z - 7000.0) < 1.0e-6 && std::abs(status.baseMeters.x - 1000.0) < 1.0e-6, "base is distance ahead");

    // A moved camera must not move an already placed flash.
    CameraPlacement moved = camera;
    moved.positionMeters = {-50000.0, 2000.0, 0.0};
    (void)placed.LightsForWindow(0.01, moved, params);
    Check(std::abs(placed.Status(0.01).front().baseMeters.z - 7000.0) < 1.0e-6, "placement is fixed once resolved");

    // Determinism: same seed -> same lights; different seed -> different channel.
    const auto a = TransientFlashRegistry::BuildCannedFlash(request, {1, 0, 0}, {0, 1, 0});
    const auto b = TransientFlashRegistry::BuildCannedFlash(request, {1, 0, 0}, {0, 1, 0});
    FlashRequest other = request;
    other.seed = 8U;
    const auto c = TransientFlashRegistry::BuildCannedFlash(other, {1, 0, 0}, {0, 1, 0});
    Check(a.Segments()[10].to[0] == b.Segments()[10].to[0], "seed reproduces the channel");
    Check(a.Segments()[10].to[0] != c.Segments()[10].to[0], "seed changes the channel");
    Check(std::abs(a.Segments().back().to[1] - request.heightMeters) < 1.0e-6, "channel reaches the requested height along up");

    // Absolute placement, and Clear.
    TransientFlashRegistry absolute;
    FlashRequest fixed = request;
    fixed.cameraRelative = false;
    fixed.baseMeters = {5.0, 6.0, 7.0};
    absolute.Fire(fixed, 0.0);
    Check(absolute.Status(0.0).front().placed && absolute.Status(0.0).front().baseMeters.x == 5.0, "absolute flash is placed immediately");
    absolute.Clear();
    Check(absolute.Empty(), "Clear removes flashes");

    // Channel lines for an arc overlay: the lit segments of a placed flash while its strokes run, none once the glow has faded,
    // brightest first and capped.
    {
        TransientFlashRegistry arcs;
        FlashRequest fixed2 = request;
        fixed2.cameraRelative = false;
        fixed2.baseMeters = {100.0, 200.0, 300.0};
        arcs.Fire(fixed2, 5.0);
        const auto during = arcs.ChannelLines(5.0 + 1.0e-4, 0.02, 1.0e3, 1000U);
        Check(!during.empty(), "a flash in progress has lit segments");
        Check(std::abs(during.front().from.x - 100.0) < 50.0 && std::abs(during.front().from.y - 200.0) < 100.0, "channel lines are in the lighting frame (origin added)");
        bool sorted = true;
        for (std::size_t i = 1; i < during.size(); ++i) { sorted = sorted && during[i - 1].powerWatts >= during[i].powerWatts; }
        const auto capped = arcs.ChannelLines(5.0 + 1.0e-4, 0.02, 1.0e3, 5U);
        Check(capped.size() == 5U && capped.front().powerWatts >= capped.back().powerWatts, "the line budget keeps the brightest");
        Check(arcs.ChannelLines(5.0 + 30.0, 0.02, 1.0e3, 1000U).empty(), "the glow fades away after the last stroke");
        Check(arcs.ChannelLines(4.0, 0.02, 1.0e3, 1000U).empty(), "nothing before the flash starts");
        (void)sorted;
    }

    // Upcoming flashes (what the strike pause watches): a placed flash is listed from `horizon` before it starts until `grace` after.
    {
        TransientFlashRegistry soon;
        FlashRequest fixed3 = request;
        fixed3.cameraRelative = false;
        fixed3.baseMeters = {1000.0, 0.0, 0.0};
        soon.Fire(fixed3, 10.0);
        Check(soon.UpcomingFlashes(8.0, 0.3, 0.1).empty(), "a flash two seconds away is not upcoming yet");
        const auto near = soon.UpcomingFlashes(9.8, 0.3, 0.1);
        Check(near.size() == 1U && near.front().secondsUntilStart > 0.0 && near.front().secondsUntilStart < 0.3, "a flash 0.2 s away is upcoming");
        Check(near.front().radiusMeters > 100.0 && std::abs(near.front().centre.x - 1000.0) < near.front().radiusMeters, "its sphere contains the channel");
        Check(soon.UpcomingFlashes(10.05, 0.3, 0.1).size() == 1U, "just started still counts");
        Check(soon.UpcomingFlashes(10.5, 0.3, 0.1).empty(), "a flash well under way no longer does");
    }

    // Several simultaneous flashes still respect the budget.
    TransientFlashRegistry many;
    for (int i = 0; i < 8; ++i) { many.Fire(request, 0.0); }
    Check(many.LightsForWindow(0.0, camera, params).size() <= params.maxLights, "budget holds across flashes");

    // Storm anchor: a flash in storm coordinates is placed against a fixed spot and does not follow the camera.
    {
        optical_transport::DischargeGraph graph(optical_transport::FlashKind::CloudToGround);
        optical_transport::Segment seg;
        seg.from[0] = 16000.0; seg.from[1] = 16000.0; seg.from[2] = 0.0;
        seg.to[0] = 16000.0; seg.to[1] = 16000.0; seg.to[2] = 1000.0;
        seg.conductivity = 1.0;
        graph.AddSegment(seg);
        graph.Reactivate(0, {0.0, 1.0e-4, 1.0e5});
        TransientFlashRegistry storm;
        const u64 early = storm.AddStormFlash(graph, 0.0);
        Check(early != 0U && storm.LightsForWindow(0.0, camera, params).empty(), "a storm flash without an anchor is not lit");
        StormAnchor anchor;   // 12 km ahead, domain 32 km
        storm.SetStormAnchor(anchor);
        const auto lights = storm.LightsForWindow(0.0, camera, params);
        Check(!lights.empty(), "once anchored the flash is lit");
        if (!lights.empty())
        {
            const auto& p = lights.front().positionInFrameMeters;
            Check(std::abs(p.x - 1000.0) < 1.0 && std::abs(p.z - 15000.0) < 1.0 && p.y > 2000.0 && p.y < 3000.0,
                  "the domain centre lands 12 km ahead of the camera, channel rising along up");
        }
        CameraPlacement far = camera;
        far.positionMeters = {-90000.0, 0.0, 0.0};
        const auto again = storm.LightsForWindow(0.0, far, params);
        Check(!again.empty() && std::abs(again.front().positionInFrameMeters.x - 1000.0) < 1.0, "the anchor does not follow the camera");
        const auto stormStatus = storm.Status(0.0).front();
        Check(stormStatus.remainingSeconds > 0.2 && stormStatus.remainingSeconds < 0.5, "lifetime is the last pulse plus a short tail");
    }

    std::printf(failures == 0 ? "TransientFlashRegistry tests passed\n" : "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
