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

    // Several simultaneous flashes still respect the budget.
    TransientFlashRegistry many;
    for (int i = 0; i < 8; ++i) { many.Fire(request, 0.0); }
    Check(many.LightsForWindow(0.0, camera, params).size() <= params.maxLights, "budget holds across flashes");

    std::printf(failures == 0 ? "TransientFlashRegistry tests passed\n" : "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
