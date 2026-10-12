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

void TransientFlashRegistry::SetStormAnchor(const StormAnchor& anchor)
{
    anchor_ = anchor;
    anchorRequested_ = true;
    anchorResolved_ = false;
}

u64 TransientFlashRegistry::AddStormFlash(optical_transport::DischargeGraph graph, const f64 nowSeconds)
{
    f64 end = 0.0;
    for (const auto& segment : graph.Segments())
    {
        for (const auto& pulse : segment.pulses)
        {
            end = std::max(end, pulse.startTime + pulse.duration);
        }
    }
    Flash flash;
    flash.id = nextId_++;
    flash.graph = std::move(graph);
    flash.startSeconds = nowSeconds;
    flash.lifetimeSeconds = end + kTailSeconds;
    flash.stormCoordinates = true;
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

    if (anchorRequested_ && !anchorResolved_)
    {
        const math::Double3 up = Normalized(camera.up, {0.0, 1.0, 0.0});
        math::Double3 forward = camera.forward - up * math::Dot(camera.forward, up);
        forward = Normalized(forward, Normalized(math::Cross(up, {1.0, 0.0, 0.0}), {0.0, 0.0, 1.0}));
        const math::Double3 right = Normalized(math::Cross(up, forward), {1.0, 0.0, 0.0});
        anchorUp_ = up;
        anchorForward_ = forward;
        anchorRight_ = right;
        // The domain's horizontal centre lands at the requested spot; local (0, 0) is a corner half a domain back and left.
        anchorOrigin_ = camera.positionMeters + forward * (anchor_.distanceMeters - anchor_.domainMeters * 0.5) +
                        right * (anchor_.lateralMeters - anchor_.domainMeters * 0.5);
        anchorResolved_ = true;
    }

    std::vector<LocalLight> all;
    for (Flash& flash : flashes_)
    {
        if (flash.stormCoordinates)
        {
            if (!flash.placed && anchorResolved_)
            {
                for (std::size_t i = 0; i < flash.graph.Segments().size(); ++i)
                {
                    optical_transport::Segment& segment = flash.graph.At(static_cast<std::int32_t>(i));
                    for (double* p : {segment.from, segment.to})
                    {
                        const math::Double3 world = anchorRight_ * p[0] + anchorForward_ * p[1] + anchorUp_ * p[2];
                        p[0] = world.x; p[1] = world.y; p[2] = world.z;
                    }
                }
                flash.origin = anchorOrigin_;
                flash.placed = true;
            }
            if (!flash.placed)
            {
                continue;
            }
        }
        else if (!flash.placed)
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

std::vector<ChannelLine> TransientFlashRegistry::ChannelLines(
    const f64 nowSeconds, const f64 afterglowSeconds, const f64 minimumPowerWatts, const std::size_t maxLines) const
{
    std::vector<ChannelLine> lines;
    const f64 glow = std::max(afterglowSeconds, 1.0e-6);
    for (const Flash& flash : flashes_)
    {
        if (!flash.placed)
        {
            continue;
        }
        const f64 t = nowSeconds - flash.startSeconds;
        for (const optical_transport::Segment& segment : flash.graph.Segments())
        {
            f64 power = 0.0;
            for (const optical_transport::Pulse& pulse : segment.pulses)
            {
                if (t < pulse.startTime || pulse.duration <= 0.0)
                {
                    continue;
                }
                const f64 end = pulse.startTime + pulse.duration;
                const f64 watts = pulse.radiantEnergy / pulse.duration;
                power += t <= end ? watts : watts * std::exp(-(t - end) / glow);
            }
            if (power >= minimumPowerWatts)
            {
                lines.push_back({flash.origin + math::Double3{segment.from[0], segment.from[1], segment.from[2]},
                                 flash.origin + math::Double3{segment.to[0], segment.to[1], segment.to[2]}, power});
            }
        }
    }
    if (lines.size() > maxLines)
    {
        std::partial_sort(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(maxLines), lines.end(),
                          [](const ChannelLine& a, const ChannelLine& b) { return a.powerWatts > b.powerWatts; });
        lines.resize(maxLines);
    }
    return lines;
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
