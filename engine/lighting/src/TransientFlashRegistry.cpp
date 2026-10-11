#include <orbit/lighting/TransientFlashRegistry.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::lighting
{
namespace
{
constexpr f64 kTailSeconds = 0.25;   // after the last stroke the flash is kept this long, then dropped

[[nodiscard]] math::Double3 Normalized(const math::Double3& v, const math::Double3& fallback)
{
    const f64 length = std::sqrt(math::Dot(v, v));
    return length > 1.0e-12 ? v * (1.0 / length) : fallback;
}

// Small deterministic generator so a seed reproduces the same channel on every machine.
[[nodiscard]] f64 Next(u32& state)
{
    state = state * 1664525U + 1013904223U;
    return static_cast<f64>(state >> 8U) / 16777216.0 - 0.5;
}

[[nodiscard]] f64 LifetimeOf(const FlashRequest& request)
{
    return request.strokeSpacingSeconds * static_cast<f64>(std::max(request.strokes, 1U) - 1U) + kTailSeconds;
}
} // namespace

optical_transport::DischargeGraph TransientFlashRegistry::BuildCannedFlash(
    const FlashRequest& request, const math::Double3& right, const math::Double3& up)
{
    using optical_transport::Segment;
    constexpr int kSegments = 24;
    constexpr f64 kStrokeSeconds = 1.0e-4;
    optical_transport::DischargeGraph graph(optical_transport::FlashKind::CloudToGround);
    const f64 height = std::max(request.heightMeters, 1.0);
    const math::Double3 fwd = Normalized(math::Cross(right, up), {0.0, 0.0, 1.0});
    u32 rng = request.seed * 2654435761U + 12345U;
    math::Double3 point{};
    const f64 step = height / kSegments;
    for (int i = 0; i < kSegments; ++i)
    {
        // Channel is built top-down in time (leader) but stored ground-up: tortuosity is a bounded lateral walk.
        const math::Double3 next = point + up * step + right * (Next(rng) * step * 0.8) + fwd * (Next(rng) * step * 0.8);
        Segment s;
        s.from[0] = point.x; s.from[1] = point.y; s.from[2] = point.z;
        s.to[0] = next.x; s.to[1] = next.y; s.to[2] = next.z;
        s.parent = i - 1;
        s.formedAt = 0.0;
        s.conductivity = 1.0;
        graph.AddSegment(s);
        point = next;
    }
    const u32 strokes = std::max(request.strokes, 1U);
    const f64 perSegment = std::max(request.energyJoules, 0.0) / (static_cast<f64>(strokes) * kSegments);
    for (u32 stroke = 0; stroke < strokes; ++stroke)
    {
        for (int i = 0; i < kSegments; ++i)
        {
            // The return stroke travels upward at roughly a third of c; spread starts so the wave front is visible in the clustering.
            graph.Reactivate(i, {stroke * request.strokeSpacingSeconds + i * 1.0e-7, kStrokeSeconds, perSegment});
        }
    }
    return graph;
}

u64 TransientFlashRegistry::Fire(const FlashRequest& request, const f64 nowSeconds)
{
    Flash flash;
    flash.id = nextId_++;
    flash.request = request;
    flash.startSeconds = nowSeconds;
    flash.lifetimeSeconds = LifetimeOf(request);
    if (!request.cameraRelative)
    {
        flash.placed = true;
        flash.origin = request.baseMeters;
        flash.graph = BuildCannedFlash(request, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0});
    }
    flashes_.push_back(std::move(flash));
    return flashes_.back().id;
}

u64 TransientFlashRegistry::Add(
    optical_transport::DischargeGraph graph, const math::Double3& originMeters, const f64 nowSeconds, const f64 lifetimeSeconds)
{
    Flash flash;
    flash.id = nextId_++;
    flash.placed = true;
    flash.origin = originMeters;
    flash.graph = std::move(graph);
    flash.startSeconds = nowSeconds;
    flash.lifetimeSeconds = lifetimeSeconds;
    flashes_.push_back(std::move(flash));
    return flashes_.back().id;
}

std::vector<LocalLight> TransientFlashRegistry::LightsForWindow(
    const f64 nowSeconds, const CameraPlacement& camera, const TransientLightParams& params)
{
    std::erase_if(flashes_, [&](const Flash& f) { return nowSeconds - f.startSeconds > f.lifetimeSeconds; });

    std::vector<LocalLight> all;
    for (Flash& flash : flashes_)
    {
        if (!flash.placed)
        {
            const math::Double3 up = Normalized(camera.up, {0.0, 1.0, 0.0});
            math::Double3 forward = camera.forward - up * math::Dot(camera.forward, up);
            forward = Normalized(forward, Normalized(math::Cross(up, {1.0, 0.0, 0.0}), {0.0, 0.0, 1.0}));
            const math::Double3 right = Normalized(math::Cross(up, forward), {1.0, 0.0, 0.0});
            flash.origin = camera.positionMeters + forward * flash.request.distanceMeters + right * flash.request.lateralMeters;
            flash.graph = BuildCannedFlash(flash.request, right, up);
            flash.placed = true;
        }
        const f64 t0 = nowSeconds - flash.startSeconds;
        auto lights = TransientLightsFromFlash(flash.graph, flash.origin, t0, params, flash.id << 8U);
        all.insert(all.end(), lights.begin(), lights.end());
    }
    if (all.size() > params.maxLights)
    {
        std::partial_sort(all.begin(), all.begin() + params.maxLights, all.end(),
                          [](const LocalLight& a, const LocalLight& b) { return a.luminousFluxLumens > b.luminousFluxLumens; });
        all.resize(params.maxLights);
    }
    return all;
}

std::vector<FlashStatus> TransientFlashRegistry::Status(const f64 nowSeconds) const
{
    std::vector<FlashStatus> result;
    for (const Flash& flash : flashes_)
    {
        result.push_back({flash.id, flash.placed, nowSeconds - flash.startSeconds,
                          std::max(0.0, flash.startSeconds + flash.lifetimeSeconds - nowSeconds), flash.origin});
    }
    return result;
}

void TransientFlashRegistry::Clear()
{
    flashes_.clear();
}
} // namespace orbit::lighting
