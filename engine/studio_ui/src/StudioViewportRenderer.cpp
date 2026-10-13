#include "StudioViewportInternals.hpp"
#include "StudioViewportPrimitives.hpp"

#include <orbit/celestial_atmosphere/AtmosphereRefraction.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/optical_transport/Blackbody.hpp>
#include <orbit/post_process/HumanEyeAdaptation.hpp>

#include <chrono>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <limits>

namespace orbit::studio_ui
{
using namespace viewport_detail;

namespace
{
// How far a node of a lightning channel (a centre of the 500 m electrification cells) is moved to hide the lattice: a quarter of a cell.
constexpr f64 kNodeJitterMeters = 125.0;

// ORBIT_REFRACT_SUN=1 turns on atmospheric refraction of the sun direction (applied where studioDirectLight is resolved). Read once.
[[nodiscard]] bool RefractSunEnabled()
{
    static const bool enabled = [] {
        char* value = nullptr;
        std::size_t length = 0;
        bool on = false;
        if (_dupenv_s(&value, &length, "ORBIT_REFRACT_SUN") == 0 && value != nullptr)
        {
            on = value[0] == '1';
            std::free(value);
        }
        return on;
    }();
    return enabled;
}
} // namespace

void StudioViewportRenderer::TickFlashClock(const time::SimulationTime atTime)
{
    const f64 wall = std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const f64 sim = static_cast<f64>(atTime.microsecondsFromEpoch) * 1.0e-6;
    if (flashLastWall_ < 0.0)
    {
        flashLastWall_ = wall;
        flashLastSim_ = sim;
        flashLastSimChangeWall_ = wall;
        return;
    }
    const f64 elapsed = std::min(wall - flashLastWall_, 0.25);   // a hitch does not jump the clock
    flashLastWall_ = wall;
    if (sim != flashLastSim_)
    {
        const f64 instant = (sim - flashLastSim_) / std::max(wall - flashLastSimChangeWall_, 1.0e-4);
        simulationRate_ = 0.5 * simulationRate_.load() + 0.5 * instant;
        flashLastSim_ = sim;
        flashLastSimChangeWall_ = wall;
    }
    else if (wall - flashLastSimChangeWall_ > 0.25)
    {
        simulationRate_ = 0.0;   // paused
    }
    const f64 before = flashClock_.load();
    flashClock_ = before + elapsed * std::clamp(simulationRate_.load(), 0.0, 1.0);
    flashWindowEnd_ = flashClock_.load();
    flashWindowStart_ = std::min(before, flashWindowEnd_ - 1.0e-3);
}

u64 StudioViewportRenderer::FireLightning(const lighting::FlashRequest& request)
{
    const std::scoped_lock lock(transientFlashMutex_);
    return transientFlashes_.Fire(request, FlashClockSeconds());
}

std::vector<lighting::FlashStatus> StudioViewportRenderer::LightningStatus() const
{
    const std::scoped_lock lock(transientFlashMutex_);
    return transientFlashes_.Status(FlashClockSeconds());
}

std::string StudioViewportRenderer::SetCloudShaderDefines(const std::string& defines)
{
    return cloudRenderer_.SetShaderDefines(defines);
}

const std::string& StudioViewportRenderer::CloudShaderDefines() const noexcept
{
    return cloudRenderer_.ShaderDefines();
}

std::string StudioViewportRenderer::SetCloudSplit(const bool enabled, const std::string& defines, const f32 position)
{
    if (!enabled)
    {
        cloudRenderer_.ClearCompare();
        return {};
    }
    return cloudRenderer_.SetCompareDefines(defines, position);
}

bool StudioViewportRenderer::CloudSplitEnabled() const noexcept
{
    return cloudRenderer_.CompareActive();
}

const std::string& StudioViewportRenderer::CloudSplitDefines() const noexcept
{
    return cloudRenderer_.CompareDefines();
}

f32 StudioViewportRenderer::CloudSplitPosition() const noexcept
{
    return cloudRenderer_.ComparePosition();
}

void StudioViewportRenderer::ClearLightning()
{
    const std::scoped_lock lock(transientFlashMutex_);
    transientFlashes_.Clear();
}

void StudioViewportRenderer::SetLightningStormAnchor(const lighting::StormAnchor& anchor)
{
    const std::scoped_lock lock(transientFlashMutex_);
    transientFlashes_.SetStormAnchor(anchor);
}

u64 StudioViewportRenderer::AddLightningStormFlash(optical_transport::DischargeGraph graph)
{
    const std::scoped_lock lock(transientFlashMutex_);
    return transientFlashes_.AddStormFlash(std::move(graph), FlashClockSeconds());
}

void StudioViewportRenderer::WatchStrikes(const render_view::CameraState& camera, const u32 width, const u32 height)
{
    const StormLightningSettings watchSettings = stormMailbox_.Lightning();
    const bool pausing = watchSettings.pauseOnStrike && static_cast<bool>(strikePauseHandler_);
    const bool framing = watchSettings.frameStrike && static_cast<bool>(strikeFrameHandler_);
    if ((!pausing && !framing) || width == 0U || height == 0U)
    {
        strikeSeen_.clear();
        return;
    }
    std::vector<lighting::UpcomingFlash> upcoming;
    {
        const std::scoped_lock lock(transientFlashMutex_);
        upcoming = transientFlashes_.UpcomingFlashes(FlashClockSeconds(), 0.3, 0.1);
    }
    const math::Double3 forward{camera.forward.x, camera.forward.y, camera.forward.z};
    const math::Double3 up{camera.up.x, camera.up.y, camera.up.z};
    const math::Double3 right = math::Cross(up, forward);
    const f64 rightLength = std::sqrt(math::Dot(right, right));
    if (rightLength <= 0.0)
    {
        return;
    }
    const math::Double3 side = right * (1.0 / rightLength);
    const f64 tanHalf = std::tan(0.5 * static_cast<f64>(camera.verticalFovRadians));
    const f64 aspect = static_cast<f64>(width) / static_cast<f64>(height);
    for (const lighting::UpcomingFlash& flash : upcoming)
    {
        if (!strikeSeen_.insert(flash.id).second)
        {
            continue;   // already looked at (and paused for, or out of frame)
        }
        bool inFrame = false;
        if (framing)
        {
            // The camera is sent to the flash: it will be in view when the request is applied (before the next frame).
            strikeFrameHandler_(flash.centre, flash.radiusMeters, static_cast<f64>(camera.verticalFovRadians), aspect);
            inFrame = true;
        }
        else
        {
            const math::Double3 rel = flash.centre - camera.localPositionMeters;
            const f64 depth = math::Dot(rel, forward);
            if (depth <= 1.0)
            {
                continue;
            }
            // Inside the view, allowing for the size of the channel.
            const f64 margin = flash.radiusMeters / depth;
            inFrame = std::fabs(math::Dot(rel, side)) / depth < tanHalf * aspect + margin &&
                      std::fabs(math::Dot(rel, up)) / depth < tanHalf + margin;
        }
        if (inFrame)
        {
            if (pausing)
            {
                strikePauseHandler_();
            }
            return;
        }
    }
    // Forget flashes that are gone, so the set does not grow.
    if (strikeSeen_.size() > 256U)
    {
        strikeSeen_.clear();
    }
}

std::vector<editor_ui::PreviewLine> StudioViewportRenderer::ElectrificationGridLines(const render_view::CameraState& camera)
{
    std::vector<editor_ui::PreviewLine> out;
    if (!stormMailbox_.Lightning().showGrid || !lightningFrame_.valid || lightningFrame_.cells == 0U)
    {
        return out;
    }
    const LightningGridFrame& f = lightningFrame_;
    const f64 width = f.cells * f.spacing;
    const f64 height = f.levels * f.spacing;
    const auto toFloat = [&](const f64 x, const f64 y, const f64 z) {
        const math::Double3 p = f.centre + f.east * (f.offsetEast + x) + f.north * (f.offsetNorth + y) + f.up * z - camera.localPositionMeters;
        return math::Float3{static_cast<f32>(p.x), static_cast<f32>(p.y), static_cast<f32>(p.z)};
    };
    const auto line = [&](const f64 x0, const f64 y0, const f64 z0, const f64 x1, const f64 y1, const f64 z1, const math::Float4& colour) {
        out.push_back({toFloat(x0, y0, z0), toFloat(x1, y1, z1), colour});
    };
    const math::Float4 edge{0.30F, 0.85F, 1.0F, 0.9F};
    const math::Float4 inner{0.30F, 0.85F, 1.0F, 0.28F};
    const f64 step = 8.0 * f.spacing;   // a grid line every 8 cells (4 km)
    // The box.
    for (const f64 z : {0.0, height})
    {
        line(0, 0, z, width, 0, z, edge);
        line(width, 0, z, width, width, z, edge);
        line(width, width, z, 0, width, z, edge);
        line(0, width, z, 0, 0, z, edge);
    }
    for (const f64 x : {0.0, width})
    {
        for (const f64 y : {0.0, width})
        {
            line(x, y, 0, x, y, height, edge);
        }
    }
    // The ground grid, level rings every 4 km, and the vertical lines of the grid on the four faces.
    for (f64 a = step; a < width - 1.0; a += step)
    {
        line(a, 0, 0, a, width, 0, inner);
        line(0, a, 0, width, a, 0, inner);
        line(a, 0, 0, a, 0, height, inner);
        line(a, width, 0, a, width, height, inner);
        line(0, a, 0, 0, a, height, inner);
        line(width, a, 0, width, a, height, inner);
    }
    for (f64 z = step; z < height - 1.0; z += step)
    {
        line(0, 0, z, width, 0, z, inner);
        line(width, 0, z, width, width, z, inner);
        line(width, width, z, 0, width, z, inner);
        line(0, width, z, 0, 0, z, inner);
    }
    // The charge: a cross on every block that holds a noticeable share of the largest one, red where positive and blue where negative,
    // its size growing with the square root of the charge.
    if (lightningBlocks_ && !lightningBlocks_->coulombs.empty())
    {
        const StormChargeBlocks& b = *lightningBlocks_;
        f32 largest = 0.0F;
        for (const f32 q : b.coulombs) { largest = std::max(largest, std::fabs(q)); }
        if (largest > 0.0F)
        {
            for (u32 k = 0; k < b.nz; ++k)
            {
                for (u32 j = 0; j < b.ny; ++j)
                {
                    for (u32 i = 0; i < b.nx; ++i)
                    {
                        const f32 q = b.coulombs[(static_cast<std::size_t>(k) * b.ny + j) * b.nx + i];
                        const f32 share = std::fabs(q) / largest;
                        if (share < 0.05F)
                        {
                            continue;
                        }
                        const f64 half = 0.5 * b.blockMeters * std::sqrt(static_cast<f64>(share));
                        const f64 cx = (i + 0.5) * b.blockMeters, cy = (j + 0.5) * b.blockMeters, cz = (k + 0.5) * b.blockMeters;
                        const math::Float4 colour = q > 0.0F ? math::Float4{1.0F, 0.35F, 0.25F, 0.95F} : math::Float4{0.30F, 0.55F, 1.0F, 0.95F};
                        line(cx - half, cy, cz, cx + half, cy, cz, colour);
                        line(cx, cy - half, cz, cx, cy + half, cz, colour);
                        line(cx, cy, cz - half, cx, cy, cz + half, colour);
                    }
                }
            }
        }
    }
    return out;
}

std::vector<editor_ui::PreviewLine> StudioViewportRenderer::FlashArcLines(const render_view::CameraState& camera, const u32 height)
{
    std::vector<editor_ui::PreviewLine> out;
    lastArcSegments_ = 0U;
    const StormLightningSettings settings = stormMailbox_.Lightning();
    if (!settings.arcs || height == 0U)
    {
        return out;
    }
    std::vector<lighting::ChannelLine> channel;
    {
        const std::scoped_lock lock(transientFlashMutex_);
        if (transientFlashes_.Empty())
        {
            return out;
        }
        // A channel stays visible a few milliseconds after its pulse (the plasma cools), as the lights on the clouds do: they read the same
        // pulses, and a longer fade made the arc outlast the light several times over.
        channel = transientFlashes_.ChannelLinesInWindow(flashWindowStart_, flashWindowEnd_, 1.0e3, 8000U);
    }
    if (channel.empty())
    {
        return out;
    }
    const math::Double3 forward{camera.forward.x, camera.forward.y, camera.forward.z};
    const math::Double3 up{camera.up.x, camera.up.y, camera.up.z};
    const math::Double3 right = math::Cross(up, forward);
    const f64 rightLength = std::sqrt(math::Dot(right, right));
    const math::Double3 side = rightLength > 0.0 ? right * (1.0 / rightLength) : math::Double3{1.0, 0.0, 0.0};
    // The width of an arc is a share of the screen height, not a number of pixels: its glow is measured on a 1080 pixel high reference screen
    // and the core is thickened by whole device pixels when the screen is higher (4K: a core of three pixels), so it looks the same at any
    // resolution.
    const f64 devicePixelAngle = 2.0 * std::tan(0.5 * static_cast<f64>(camera.verticalFovRadians)) / static_cast<f64>(height);
    const f64 pixelAngle = 2.0 * std::tan(0.5 * static_cast<f64>(camera.verticalFovRadians)) / 1080.0;
    const int extraCore = std::clamp(static_cast<int>(std::ceil(0.5 * (static_cast<f64>(height) / 1080.0 - 1.0) - 1.0e-6)), 0, 4);
    // Cloud in front of a channel: the extinction of the storm's window is integrated along the line of sight to the middle of the segment.
    const std::shared_ptr<const StormChargeBlocks> blocks = lightningBlocks_;
    const LightningGridFrame frame = lightningFrame_;
    const bool occlude = blocks && frame.valid && !blocks->extinction.empty() && blocks->extinctionCells > 0U;
    // Light is scattered forward in a cloud, so a channel stays visible through a much larger optical depth than the extinction suggests.
    constexpr f64 kForwardScatterShare = 0.12;
    const auto transmittance = [&](const math::Double3& target) {
        if (!occlude) { return 1.0; }
        const math::Double3 start = camera.localPositionMeters;
        const math::Double3 ray = target - start;
        const f64 distance = std::sqrt(math::Dot(ray, ray));
        const f64 step = 0.5 * static_cast<f64>(blocks->extinctionSpacing);
        const u32 samples = std::min(static_cast<u32>(distance / std::max(step, 1.0)) + 1U, 400U);
        f64 optical = 0.0;
        const f64 stride = distance / samples;
        for (u32 s = 0; s < samples; ++s)
        {
            const math::Double3 rel = start + ray * ((s + 0.5) / samples) - frame.centre;
            const f64 x = math::Dot(rel, frame.east) - frame.offsetEast, y = math::Dot(rel, frame.north) - frame.offsetNorth, z = math::Dot(rel, frame.up);
            const f64 inv = 1.0 / static_cast<f64>(blocks->extinctionSpacing);
            if (x < 0.0 || y < 0.0 || z < 0.0) { continue; }
            const u32 i = static_cast<u32>(x * inv), j = static_cast<u32>(y * inv), k = static_cast<u32>(z * inv);
            if (i >= blocks->extinctionCells || j >= blocks->extinctionCells || k >= blocks->extinctionLevels) { continue; }
            optical += static_cast<f64>(blocks->extinction[(static_cast<std::size_t>(k) * blocks->extinctionCells + j) * blocks->extinctionCells + i]) * stride;
        }
        return std::exp(-kForwardScatterShare * optical);
    };
    const auto toFloat = [](const math::Double3& v) { return math::Float3{static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)}; };
    lastArcSegments_ = static_cast<u32>(channel.size());
    out.reserve(channel.size() * 12U);
    // The electrification's channel follows the 500 m grid, so its segments are straight and axis aligned. Each one is broken into four
    // with a lateral displacement taken from a hash of its own end points: jagged like a real channel, the same every frame, and the
    // neighbouring segments still meet at the shared end points.
    const auto unit = [](u64 h) {
        h ^= h >> 33U; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33U; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33U;
        return static_cast<f64>(h >> 11U) / 9007199254740992.0;
    };
    const auto quantised = [](const f64 v) { return static_cast<u64>(static_cast<i64>(std::llround(v))); };
    for (const lighting::ChannelLine& line : channel)
    {
        const math::Double3 from = line.from - camera.localPositionMeters;
        const math::Double3 to = line.to - camera.localPositionMeters;
        const math::Double3 mid = (from + to) * 0.5;
        const f64 distance = std::sqrt(math::Dot(mid, mid));
        if (math::Dot(mid, forward) <= 0.0)
        {
            continue;   // behind the camera
        }
        const f64 pixel = distance * pixelAngle;
        // Brightness from the optical power per metre of the segment (without the brightness gain, which is for the lights): on a power law
        // with 1e3 W/m (the cooled channel) at 0.25, 2e4 (a leader) 0.8, 2.5e5 (a later stroke) 2.3 and 1e6 (a first stroke) 4, clamped at 12;
        // the colour is the blackbody colour of the segment's temperature, blue-white when hot and orange when it has cooled.
        const f64 perMeter = line.powerWatts / std::max(settings.brightness, 1.0) / std::max(line.lengthMeters, 1.0);
        const f64 seen = transmittance((line.from + line.to) * 0.5);
        if (seen < 0.01) { continue; }   // buried in the cloud
        const f64 level = std::clamp(0.25 * std::pow(std::max(perMeter, 1.0) / 1.0e3, 0.4), 0.03, 12.0) * seen;
        const std::array<f64, 3> colour = line.temperatureKelvin > 0.0 ? optical_transport::BlackbodyLinearRgb(line.temperatureKelvin)
                                                                       : std::array<f64, 3>{0.80, 0.87, 1.0};
        const f64 trunk = std::clamp(std::pow(perMeter / 1.0e6, 0.25), 0.0, 1.0);   // 1 at a first stroke's power, 0.37 at a leader's
        const f32 coreAlpha = static_cast<f32>(0.3 + 0.7 * trunk);
        // The electrification's cells make a lattice; every node (a cell centre) is moved by a hash of its own position, so the segments that
        // meet in it still meet, and the lattice is not recognisable.
        const auto nodeShift = [&](const math::Double3& world) {
            u64 h = 0x9E3779B97F4A7C15ULL;
            for (const f64 v : {world.x, world.y, world.z}) { h = (h ^ quantised(v)) * 0x100000001B3ULL + 0x7F4A7C15ULL; }
            return math::Double3{(unit(h) - 0.5) * 2.0 * kNodeJitterMeters, (unit(h + 1U) - 0.5) * 2.0 * kNodeJitterMeters,
                                 (unit(h + 2U) - 0.5) * 2.0 * kNodeJitterMeters};
        };
        const math::Double3 fromJittered = from + nodeShift(line.from);
        const math::Double3 toJittered = to + nodeShift(line.to);
        const math::Double3 direction = toJittered - fromJittered;
        const f64 length = std::sqrt(math::Dot(direction, direction));
        math::Double3 points[5] = {fromJittered, {}, {}, {}, toJittered};
        if (length > 1.0)
        {
            const math::Double3 axis = direction * (1.0 / length);
            const math::Double3 helper = std::fabs(axis.y) < 0.9 ? math::Double3{0.0, 1.0, 0.0} : math::Double3{1.0, 0.0, 0.0};
            math::Double3 p1 = math::Cross(axis, helper);
            p1 = p1 * (1.0 / std::sqrt(math::Dot(p1, p1)));
            const math::Double3 p2 = math::Cross(axis, p1);
            u64 seed = 0x9E3779B97F4A7C15ULL;
            for (const f64 v : {line.from.x, line.from.y, line.from.z, line.to.x, line.to.y, line.to.z})
            {
                seed = (seed ^ quantised(v)) * 0x100000001B3ULL + 0x7F4A7C15ULL;
            }
            const auto jitter = [&](const u64 k, const f64 amount) {
                return p1 * ((unit(seed + 2U * k) - 0.5) * 2.0 * amount * length) + p2 * ((unit(seed + 2U * k + 1U) - 0.5) * 2.0 * amount * length);
            };
            const math::Double3 middle = (fromJittered + toJittered) * 0.5;
            points[2] = middle + jitter(1U, 0.30);
            points[1] = (fromJittered + points[2]) * 0.5 + jitter(2U, 0.15);
            points[3] = (points[2] + toJittered) * 0.5 + jitter(3U, 0.15);
        }
        else
        {
            points[1] = fromJittered * 0.75 + toJittered * 0.25;
            points[2] = (fromJittered + toJittered) * 0.5;
            points[3] = fromJittered * 0.25 + toJittered * 0.75;
        }
        for (int k = 0; k < 4; ++k)
        {
            const auto add = [&](const math::Double3& offset, const f32 alpha) {
                out.push_back({toFloat(points[k] + offset), toFloat(points[k + 1] + offset),
                               {static_cast<f32>(colour[0] * level), static_cast<f32>(colour[1] * level), static_cast<f32>(colour[2] * level), alpha}});
            };
            add({}, coreAlpha);
            if (trunk > 0.3)
            {
                const f64 devicePixel = distance * devicePixelAngle;
                for (int e = 1; e <= extraCore; ++e)
                {
                    add(side * (devicePixel * e), coreAlpha);
                    add(side * (-devicePixel * e), coreAlpha);
                }
            }
            // A soft edge: two lines half a pixel to each side (not on the faint twigs, of which there are thousands); the strong segments
            // (the trunk) get a wider glow.
            if (trunk > 0.5)
            {
                add(side * (pixel * 0.6), 0.4F);
                add(side * (-pixel * 0.6), 0.4F);
            }
            if (trunk > 0.75)
            {
                add(side * (pixel * 1.4), 0.3F);
                add(side * (-pixel * 1.4), 0.3F);
            }
            if (trunk > 0.97)
            {
                add(side * (pixel * 2.4), 0.25F);
                add(side * (-pixel * 2.4), 0.25F);
            }
        }
    }
    return out;
}

std::vector<lighting::LocalLight> StudioViewportRenderer::TransientFlashLights(const lighting::CameraPlacement& camera)
{
    const std::scoped_lock lock(transientFlashMutex_);
    if (transientFlashes_.Empty())
    {
        return {};
    }
    lighting::TransientLightParams params;
    params.exposureSeconds = std::max(flashWindowEnd_ - flashWindowStart_, 1.0e-3);
    return transientFlashes_.LightsForWindow(flashWindowStart_, camera, params);
}

StudioViewportRenderer::StudioViewportRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler,
    const u32 framesInFlight)
    : device_(&device),
      compiler_(&compiler),
      framesInFlight_(framesInFlight),
      bodyRenderer_(device, compiler),
      macroGlobeRenderer_(device, compiler),
      farBodyRenderer_(device, compiler),
      ringRenderer_(device, compiler),
      auroraRenderer_(device, compiler),
      compactObjectRenderer_(device, compiler),
      atmosphereRenderer_(device, compiler),
      cloudRenderer_(device, compiler),
      pathRenderer_(device, compiler),
      surfaceVolumeDebugRenderer_(device, compiler),
      universalVolumeRenderer_(device, compiler),
      volumeParticleRenderer_(device, compiler, framesInFlight),
      debugComposite_(device, compiler),
      directLightingRenderer_(device, compiler),
      materialEmissionSurfaceOverride_(device, compiler),
      finalGatherRenderer_(device, compiler),
      radianceCacheSampler_(device, compiler),
      proxySunShadowRenderer_(device, compiler),
      proxySurfaceRenderer_(device, compiler),
      meshLibrary_(std::make_unique<mesh_render::MeshLibrary>(device)),
      meshSurfaceRenderer_(device, compiler),
      glassSurfaceRenderer_(device, compiler),
      emissiveLightRenderer_(device, compiler),
      meshSdfScene_(device, compiler),
      meshSdfDebugRenderer_(device, compiler),
      meshShadowMapRenderer_(device, compiler),
      meshSunShadowRenderer_(device, compiler),
      antiAliasingRenderer_(device, compiler),
      hybridReflectionRenderer_(device, compiler),
      surfaceDebugRenderer_(device, compiler),
      flatMapRenderer_(device, compiler, framesInFlight),
      luminanceHistogramRenderer_(device, compiler),
      highlightEffectsRenderer_(device, compiler),
      displayResolveRenderer_(device, compiler),
      colorLutRenderer_(device, compiler),
      outputTransformRenderer_(device, compiler),
      colorLut_(
          std::make_unique<
              post_process::GpuColorLut>(
                  device,
                  post_process::
                      BuildIdentityColorLut()))
{
    meshSurfaceRenderer_.SetSdfScene(&meshSdfScene_);

    if (framesInFlight_ == 0U)
    {
        throw std::invalid_argument(
            "Studio terrain rendering requires at least one frame-in-flight slot.");
    }
}

void StudioViewportRenderer::SetContentService(
    content::ContentService* const content) noexcept
{
    content_ = content;
}

void StudioViewportRenderer::SetVolumeFieldStorageService(
    volume_fields::VolumeFieldStorageService* const fields) noexcept
{
    volumeFields_ = fields;
}

void StudioViewportRenderer::SetSurfaceVolumeSolverService(
    volume_solver::SurfaceVolumeSolverService* const solver) noexcept
{
    surfaceVolumeSolver_ = solver;
}

volume_render::VolumeRenderRuntimeSettings&
StudioViewportRenderer::VolumeRenderSettings(
    const scene::ObjectId volume)
{
    return
        universalVolumeRenderer_.
            Settings(volume);
}

volume_render::VolumeRenderDiagnostics
StudioViewportRenderer::VolumeRenderDiagnostics(
    const scene::ObjectId volume) const noexcept
{
    return
        universalVolumeRenderer_.
            Diagnostics(volume);
}

void StudioViewportRenderer::SetVolumeSourceDebugVisualization(
    const bool enabled) noexcept
{
    volumeSourceDebugVisualization_ =
        enabled;
}

bool StudioViewportRenderer::VolumeSourceDebugVisualization() const noexcept
{
    return volumeSourceDebugVisualization_;
}

std::vector<StudioRenderedView>
StudioViewportRenderer::ComposeBase(
    render_graph::RenderGraph& graph,
    StudioRenderViewSet& views,
    studio_session::StudioSession& session,
    studio_session::StudioRuntimeBinding& runtime,
    const studio_session::StudioRuntimeSnapshot& snapshot,
    const time::SimulationTime atTime,
    const bool drawPathDebug,
    const u32 frameIndex,
    const lighting::LightingWorkPlan& lightingPlan,
    lighting::LightingTimestampRecorder* const lightingTimestamps,
    const StudioComposeCpuTimingRecorder& cpuTimingRecorder)
{
    static_cast<void>(views.Refresh(snapshot));

    const universe::BodyRegistry* bodies = nullptr;
    const frames::FrameGraph* frames = nullptr;
    std::vector<const path_geometry::PathDerivedProduct*> products;

    if (snapshot.hasWorld)
    {
        bodies = &runtime.Bodies(snapshot);
        frames = &runtime.Frames(snapshot);
        products = runtime.PathProducts(snapshot).Products();
    }

    const auto celestialFrameGrants =
        celestialScheduler_.
            BuildFramePlan();

    std::vector<bool>
        celestialGrantConsumed(
            celestialFrameGrants.size(),
            false);

    const auto acquireCelestialGrant =
        [&](const std::string_view viewportId,
            const universe::BodyId body,
            const celestial_scheduler::WorkKind kind,
            const u64 authorityRevision,
            const celestial_scheduler::WorkBackend backend,
            const u32 costUnits,
            const i32 priority,
            const bool visible)
        {
            const auto key =
                CelestialWorkKeyFor(
                    viewportId,
                    body,
                    kind);

            for (std::size_t index = 0U;
                 index <
                     celestialFrameGrants.size();
                 ++index)
            {
                if (celestialGrantConsumed[index])
                {
                    continue;
                }

                const auto& grant =
                    celestialFrameGrants[index];

                if (grant.key == key &&
                    grant.authorityRevision ==
                        authorityRevision)
                {
                    celestialGrantConsumed[index] =
                        true;
                    return true;
                }
            }

            celestialScheduler_.Enqueue({
                .key = key,
                .authorityRevision =
                    authorityRevision,
                .backend = backend,
                .costUnits =
                    std::max(
                        costUnits,
                        1U),
                .priority = priority,
                .visible = visible
            });

            return false;
        };

    const auto completeCelestialGrant =
        [&](const std::string_view viewportId,
            const universe::BodyId body,
            const celestial_scheduler::WorkKind kind,
            const u64 authorityRevision)
        {
            return
                celestialScheduler_.Complete(
                    CelestialWorkKeyFor(
                        viewportId,
                        body,
                        kind),
                    authorityRevision);
        };

    std::vector<StudioRenderedView> rendered;
    auto catalog = views.Catalog();
    std::stable_sort(
        catalog.begin(),
        catalog.end(),
        [](const StudioRenderViewInfo& a,
           const StudioRenderViewInfo& b)
        {
            return a.id == "studio.primary" &&
                b.id != "studio.primary";
        });
    rendered.reserve(catalog.size());

    std::optional<world_model::ResolvedVolumeDomain> selectedVolume;
    volume_fields::VolumeFieldStorage* sharedVolumeStorage = nullptr;
    std::optional<volume_fields::ImportedVolumeFields> sharedVolumeFields;

    if (volumeFields_ != nullptr && snapshot.hasWorld &&
        session.World().Selection().Ordered().size() == 1U)
    {
        auto volumeObject = session.World().Selection().Ordered().front();
        const auto selectedRecord = session.World().Objects().Find(volumeObject);
        if (selectedRecord.has_value() && selectedRecord->parent.has_value() &&
            (selectedRecord->type == world_model::kVolumeSourceType ||
             selectedRecord->type == world_model::kVolumeEffectorType))
        {
            volumeObject = *selectedRecord->parent;
        }
        selectedVolume = world_model::ResolveVolumeDomain(
            session.World().Objects(), volumeObject);
    }

    for (const auto& info : catalog)
    {
        if (!views.CompositionEnabled(info.id))
        {
            continue;
        }

        auto composeStageStarted = std::chrono::steady_clock::now();
        const auto recordComposeStage =
            [&](const std::string_view stage)
            {
                if (!cpuTimingRecorder)
                {
                    return;
                }
                const auto now = std::chrono::steady_clock::now();
                const double stageMs =
                    std::chrono::duration<double, std::milli>(
                        now - composeStageStarted).count();
                cpuTimingRecorder(stage, stageMs);
                if (orbit::profiler::Enabled())
                {
                    const auto end = orbit::profiler::NowTicks();
                    const auto width = static_cast<u64>(
                        stageMs * orbit::profiler::TicksPerMillisecond());
                    orbit::profiler::RecordLaneSpan(
                        "Compose stages",
                        orbit::profiler::Intern(stage),
                        end > width ? end - width : 0U,
                        end);
                }
                composeStageStarted = now;
            };
        auto* view = views.Find(info.id);

        if (view == nullptr)
        {
            throw std::logic_error(
                "Studio RenderView catalog contains a missing view.");
        }

        // Anti-aliasing camera jitter. The camera is re-derived from
        // navigation every frame; if it is still exactly what we jittered last
        // frame, start again from the un-jittered camera so jitter never
        // accumulates, then apply this frame's sub-pixel rotation.
        const auto antiAliasingMode =
            static_cast<post_process::AntiAliasingMode>(
                std::min<u8>(info.layers.antiAliasing, 2U));
        {
            auto& aa = antiAliasingPresentations_[info.id];
            auto& camera = view->Camera();

            const auto sameCamera =
                [](const render_view::CameraState& a,
                   const render_view::CameraState& b)
            {
                return a.forward.x == b.forward.x &&
                       a.forward.y == b.forward.y &&
                       a.forward.z == b.forward.z &&
                       a.up.x == b.up.x && a.up.y == b.up.y &&
                       a.up.z == b.up.z &&
                       a.localPositionMeters.x == b.localPositionMeters.x &&
                       a.localPositionMeters.y == b.localPositionMeters.y &&
                       a.localPositionMeters.z == b.localPositionMeters.z &&
                       a.verticalFovRadians == b.verticalFovRadians;
            };

            if (aa.hasBase && sameCamera(camera, aa.applied))
            {
                camera = aa.base;
            }
            aa.base = camera;
            aa.hasBase = true;

            if (antiAliasingMode == post_process::AntiAliasingMode::Taa &&
                view->SurfaceDebugMode() == lighting::SurfaceDebugMode::Lit)
            {
                post_process::ApplyCameraJitter(
                    camera.forward,
                    camera.up,
                    camera.verticalFovRadians,
                    view->Height(),
                    {post_process::TaaJitterPixels(aa.frameCounter)[0] *
                         info.layers.taaJitterScale,
                     post_process::TaaJitterPixels(aa.frameCounter)[1] *
                         info.layers.taaJitterScale},
                    camera.forward,
                    camera.up);
            }
            ++aa.frameCounter;
            aa.applied = camera;
        }

        const std::string prefix =
            "StudioViewport." + info.id;
        const auto targets =
            view->Import(
                graph,
                prefix.c_str());

        const auto* logicalTarget =
            session.Viewports().Find(info.id);

        if (logicalTarget == nullptr)
        {
            throw std::logic_error(
                "Studio RenderView has no logical viewport target.");
        }

        // Per-view diagnostics are rebuilt from the current target every frame.
        stellarDiagnostics_.erase(
            info.id);
        smallBodyDiagnostics_.erase(
            info.id);
        compactObjectDiagnostics_.erase(
            info.id);

        std::optional<universe::BodyShape> shape;

        if (logicalTarget->target.has_value())
        {
            if (!snapshot.hasWorld || bodies == nullptr ||
                logicalTarget->target->universeGeneration !=
                    snapshot.universeGeneration)
            {
                throw std::logic_error(
                    "Studio viewport render target is stale for the current universe generation.");
            }

            const auto* body =
                bodies->FindBody(
                    logicalTarget->target->body);

            if (body == nullptr)
            {
                throw std::logic_error(
                    "Studio viewport target body is missing from the current BodyRegistry.");
            }

            shape = body->shape;
        }

        {
            const auto previousLightingView =
                view->Lighting();

            lighting::LightingView currentLightingView =
                previousLightingView;

            currentLightingView.frame =
                view->Camera().frame;
            currentLightingView.body =
                logicalTarget->target.has_value()
                    ? logicalTarget->target->body
                    : universe::BodyId{};
            currentLightingView.cameraPositionInFrameMeters =
                view->Camera().localPositionMeters;

            // Orbit's current renderers are camera-relative. Moving this
            // presentation origin with the camera must never alter stable GI
            // cache identity; gpuOriginRevision is reserved for a discrete
            // floating-origin rebase event when that service is introduced.
            currentLightingView.gpuOriginInFrameMeters =
                view->Camera().localPositionMeters;
            currentLightingView.forward =
                view->Camera().forward;
            currentLightingView.up =
                view->Camera().up;
            currentLightingView.verticalFovRadians =
                view->Camera().verticalFovRadians;
            currentLightingView.nearPlaneMeters =
                view->Camera().nearPlaneMeters;
            currentLightingView.farPlaneMeters =
                view->Camera().farPlaneMeters;

            currentLightingView.change =
                lighting::ClassifyLightingViewChange(
                    previousLightingView,
                    currentLightingView,
                    snapshot.viewportTargetsChanged,
                    false);

            view->Lighting() =
                currentLightingView;
        }

        if (logicalTarget->target.has_value() &&
            snapshot.hasWorld &&
            bodies != nullptr &&
            frames != nullptr)
        {
            const auto targetBody =
                logicalTarget->target->body;

            const auto* bodyRecord =
                bodies->FindBody(
                    targetBody);

            const auto bodyObject =
                session.World().
                    Universe().
                    ObjectForBody(
                        targetBody);

            if (bodyRecord != nullptr &&
                bodyObject.has_value())
            {
                const u64 semanticRevision =
                    session.World().
                        Objects().
                        PreviewRevision();

                auto& proxyPresentation =
                    visibilityProxyPresentations_[
                        info.id];

                // The GPU scenes are float32 relative to the origin they were
                // built at; refresh it when the camera has drifted away while
                // it is near the proxies (see ProxyGpuOriginIsStale).
                const bool gpuOriginStale =
                    proxyPresentation.surfaces.Ready() &&
                    lighting::ProxyGpuOriginIsStale(
                        view->Lighting().cameraPositionInFrameMeters,
                        proxyPresentation.surfaces.
                            GpuOriginInFrameMeters(),
                        proxyPresentation.surfaces.
                            CentroidInFrameMeters(),
                        proxyPresentation.surfaces.
                            BoundingRadiusMeters());

                const bool requiresRebuild =
                    gpuOriginStale ||
                    proxyPresentation.provider ==
                        nullptr ||
                    proxyPresentation.body !=
                        targetBody ||
                    proxyPresentation.frame !=
                        view->Lighting().frame ||
                    proxyPresentation.
                            semanticRevision !=
                        semanticRevision ||
                    proxyPresentation.hasDynamic;

                if (requiresRebuild)
                {
                    const auto resolved =
                        world_model::
                            ResolveVisibilityProxies(
                                session.World().
                                    Objects(),
                                *bodyObject);

                    const auto proxies =
                        BuildVisibilityProxies(
                            resolved,
                            targetBody,
                            bodyRecord->frame);

                    const bool hasDynamic =
                        std::any_of(
                            proxies.begin(),
                            proxies.end(),
                            [](const auto& proxy)
                            {
                                return proxy.dynamic;
                            });

                    proxyPresentation.scene.Rebuild(
                        proxies,
                        view->Lighting().frame,
                        *frames,
                        atTime);

                    proxyPresentation.body =
                        targetBody;
                    proxyPresentation.frame =
                        view->Lighting().frame;
                    proxyPresentation.semanticRevision =
                        semanticRevision;
                    proxyPresentation.hasDynamic =
                        hasDynamic;

                    proxyPresentation.provider =
                        std::make_unique<
                            lighting::
                                SoftwareProxyVisibilityProvider>(
                                    proxyPresentation.scene);

                    if (proxyPresentation.hardware ==
                            nullptr &&
                        device_ != nullptr &&
                        compiler_ != nullptr)
                    {
                        proxyPresentation.hardware =
                            std::make_unique<
                                lighting::
                                    HardwareRayQueryVisibilityBatch>(
                                        *device_,
                                        *compiler_);
                    }

                    if (proxyPresentation.hardware !=
                        nullptr)
                    {
                        proxyPresentation.hardware->
                            RebuildScene(
                                proxyPresentation.scene,
                                view->Lighting().
                                    gpuOriginInFrameMeters);
                    }

                    // The visible proxy surfaces need only the primitive
                    // buffer, so they do not depend on ray-query support.
                    proxyPresentation.surfaces.Rebuild(
                        *device_,
                        proxyPresentation.scene,
                        view->Lighting().
                            gpuOriginInFrameMeters);

                    const auto& stats =
                        proxyPresentation.scene.
                            Stats();

                    visibilityProxyDiagnostics_.
                        insert_or_assign(
                            info.id,
                            StudioVisibilityProxyDiagnostics{
                                .body =
                                    targetBody,
                                .frame =
                                    view->Lighting().
                                        frame,
                                .semanticRevision =
                                    semanticRevision,
                                .proxyCount =
                                    stats.proxyCount,
                                .bvhNodeCount =
                                    stats.nodeCount,
                                .dynamicProxyCount =
                                    stats.dynamicProxyCount,
                                .maximumNominalErrorMeters =
                                    stats.
                                        maximumNominalErrorMeters,
                                .rebuiltThisFrame =
                                    true,
                                .hardwareRayQuerySupported =
                                    proxyPresentation.hardware !=
                                        nullptr &&
                                    proxyPresentation.hardware->
                                        Supported(),
                                .hardwareRayQueryReady =
                                    proxyPresentation.hardware !=
                                        nullptr &&
                                    proxyPresentation.hardware->
                                        Ready(),
                                .hardwarePrimitiveCount =
                                    proxyPresentation.hardware !=
                                        nullptr
                                        ? proxyPresentation.hardware->
                                              PrimitiveCount()
                                        : 0U
                            });
                }
                else
                {
                    auto diagnostics =
                        visibilityProxyDiagnostics_[
                            info.id];

                    diagnostics.rebuiltThisFrame =
                        false;

                    visibilityProxyDiagnostics_.
                        insert_or_assign(
                            info.id,
                            diagnostics);
                }
            }
            else
            {
                visibilityProxyPresentations_.
                    erase(info.id);
                visibilityProxyDiagnostics_.
                    erase(info.id);
            }
        }
        else
        {
            visibilityProxyPresentations_.
                erase(info.id);
            visibilityProxyDiagnostics_.
                erase(info.id);
        }

        // Imported Static Meshes of the target body: placed every frame in
        // double precision relative to the camera, drawn by the mesh surface
        // pass. Models load on a worker thread and appear once resident.
        {
            auto& meshPresentation = staticMeshPresentations_[info.id];
            meshPresentation.instances.clear();
            meshPresentation.glass.clear();
            meshPresentation.emitters.clear();
            meshPresentation.requested = 0U;
            meshPresentation.hasAnchor = false;

            if (logicalTarget->target.has_value() &&
                snapshot.hasWorld &&
                bodies != nullptr &&
                frames != nullptr)
            {
                const auto meshBody = logicalTarget->target->body;
                const auto* meshBodyRecord = bodies->FindBody(meshBody);
                const auto meshBodyObject =
                    session.World().Universe().ObjectForBody(meshBody);

                if (meshBodyRecord != nullptr && meshBodyObject.has_value())
                {
                    const auto targetFromBody =
                        frames->ResolveTransform(
                            meshBodyRecord->frame,
                            view->Lighting().frame,
                            atTime);

                    if (targetFromBody.has_value())
                    {
                        const auto projectRoot =
                            session.World().Project().RootDirectory()
                                .lexically_normal();
                        const auto& cameraInFrame =
                            view->Lighting().cameraPositionInFrameMeters;
                        f64 nearestMeshDistance =
                            std::numeric_limits<f64>::infinity();

                        for (const auto& mesh :
                             world_model::ResolveStaticMeshes(
                                 session.World().Objects(),
                                 *meshBodyObject))
                        {
                            ++meshPresentation.requested;

                            // Asset paths are project-relative and may not
                            // leave the project folder.
                            const auto absolute =
                                (projectRoot /
                                 std::filesystem::path(mesh.meshAsset))
                                    .lexically_normal();
                            if (absolute.string().rfind(
                                    projectRoot.string(), 0U) != 0U)
                            {
                                continue;
                            }

                            const mesh_render::MeshModel* model =
                                meshLibrary_->Acquire(absolute);
                            if (model == nullptr)
                            {
                                continue;
                            }

                            // (Not math::Compose: this translation unit
                            // renames that identifier for the renderer.)
                            const auto placementRotation = math::Multiply(
                                targetFromBody->rotation,
                                EulerDegreesToRotation(mesh.eulerDegrees));
                            const auto placementOrigin = math::TransformPoint(
                                *targetFromBody, mesh.positionMeters);

                            if (!meshPresentation.hasAnchor)
                            {
                                meshPresentation.hasAnchor = true;
                                meshPresentation.anchorInFrame =
                                    placementOrigin;
                                meshPresentation.targetFromBody =
                                    *targetFromBody;
                            }

                            meshPresentation.instances.push_back({
                                .model = model,
                                .rows = mesh_render::MakeInstanceRows(
                                    placementRotation,
                                    mesh.uniformScale,
                                    placementOrigin - cameraInFrame)});

                            // Distance from the camera to the mesh's
                            // oriented bounding box (0 inside it).
                            const math::Double3 local =
                                math::TransformVector(
                                    math::Transpose(placementRotation),
                                    cameraInFrame - placementOrigin) *
                                (1.0 / mesh.uniformScale);
                            const auto& lo = model->BoundsMin();
                            const auto& hi = model->BoundsMax();
                            const f64 dx = std::max(
                                {lo[0] - local.x, 0.0, local.x - hi[0]});
                            const f64 dy = std::max(
                                {lo[1] - local.y, 0.0, local.y - hi[1]});
                            const f64 dz = std::max(
                                {lo[2] - local.z, 0.0, local.z - hi[2]});
                            nearestMeshDistance = std::min(
                                nearestMeshDistance,
                                std::sqrt(dx * dx + dy * dy + dz * dz) *
                                    mesh.uniformScale);
                        }

                        // Primitives (box, sphere, cylinder, capsule, plane)
                        // use the same instance path as imported meshes; their
                        // geometry is generated at the authored size, so the
                        // instance scale is 1. Glass is collected separately.
                        for (const auto& primitive :
                             world_model::ResolvePrimitives(
                                 session.World().Objects(),
                                 *meshBodyObject))
                        {
                            ++meshPresentation.requested;

                            const auto request =
                                MakePrimitiveModelRequest(primitive);
                            const mesh_render::MeshModel* model =
                                meshLibrary_->AcquireGenerated(
                                    request.key, request.build);
                            if (model == nullptr)
                            {
                                continue;
                            }

                            const auto placementRotation = math::Multiply(
                                targetFromBody->rotation,
                                EulerDegreesToRotation(
                                    primitive.eulerDegrees));
                            const auto placementOrigin = math::TransformPoint(
                                *targetFromBody, primitive.positionMeters);
                            const auto rows = mesh_render::MakeInstanceRows(
                                placementRotation,
                                1.0,
                                placementOrigin - cameraInFrame);

                            if (primitive.surface ==
                                world_model::PrimitiveSurface::Glass)
                            {
                                const auto f = [](const f64 value)
                                {
                                    return static_cast<f32>(value);
                                };
                                // A plane becomes a thin pane so it refracts.
                                const bool pane =
                                    primitive.shape ==
                                    world_model::PrimitiveShape::Plane;
                                meshPresentation.glass.push_back({
                                    .model = model,
                                    .rows = rows,
                                    .shape = static_cast<
                                        mesh_render::GlassShape>(
                                        primitive.shape),
                                    .halfExtents =
                                        {f(primitive.sizeMeters.x * 0.5),
                                         pane
                                             ? mesh_render::
                                                   kGlassPaneHalfThicknessMeters
                                             : f(primitive.sizeMeters.y * 0.5),
                                         f(primitive.sizeMeters.z * 0.5)},
                                    .tint = {f(primitive.color.x),
                                             f(primitive.color.y),
                                             f(primitive.color.z)},
                                    .indexOfRefraction =
                                        f(primitive.indexOfRefraction),
                                    .caustics = primitive.caustics,
                                    .castShadows = primitive.castShadows});
                            }
                            else
                            {
                                if (!meshPresentation.hasAnchor)
                                {
                                    meshPresentation.hasAnchor = true;
                                    meshPresentation.anchorInFrame =
                                        placementOrigin;
                                    meshPresentation.targetFromBody =
                                        *targetFromBody;
                                }
                                meshPresentation.instances.push_back(
                                    {.model = model, .rows = rows});

                                if (primitive.surface ==
                                        world_model::PrimitiveSurface::Emissive &&
                                    primitive.emissionNits > 0.0)
                                {
                                    const std::array<f32, 3> half{
                                        static_cast<f32>(
                                            primitive.sizeMeters.x * 0.5),
                                        static_cast<f32>(
                                            primitive.sizeMeters.y * 0.5),
                                        static_cast<f32>(
                                            primitive.sizeMeters.z * 0.5)};
                                    const f32 radiance = static_cast<f32>(
                                        primitive.emissionNits /
                                        static_cast<f64>(
                                            post_process::
                                                kSceneLuminanceNitsPerUnit));
                                    const auto relative =
                                        placementOrigin - cameraInFrame;
                                    meshPresentation.emitters.push_back({
                                        .position =
                                            {static_cast<f32>(relative.x),
                                             static_cast<f32>(relative.y),
                                             static_cast<f32>(relative.z)},
                                        .radius =
                                            mesh_render::EquivalentSphereRadius(
                                                mesh_render::EmissiveSurfaceArea(
                                                    static_cast<u32>(
                                                        primitive.shape),
                                                    half)),
                                        .boundingRadius = std::sqrt(
                                            half[0] * half[0] +
                                            half[1] * half[1] +
                                            half[2] * half[2]),
                                        .radiance =
                                            {static_cast<f32>(
                                                 primitive.color.x) * radiance,
                                             static_cast<f32>(
                                                 primitive.color.y) * radiance,
                                             static_cast<f32>(
                                                 primitive.color.z) * radiance}});
                                }
                            }

                            const math::Double3 local =
                                math::TransformVector(
                                    math::Transpose(placementRotation),
                                    cameraInFrame - placementOrigin);
                            const auto& lo = model->BoundsMin();
                            const auto& hi = model->BoundsMax();
                            const f64 dx = std::max(
                                {lo[0] - local.x, 0.0, local.x - hi[0]});
                            const f64 dy = std::max(
                                {lo[1] - local.y, 0.0, local.y - hi[1]});
                            const f64 dz = std::max(
                                {lo[2] - local.z, 0.0, local.z - hi[2]});
                            nearestMeshDistance = std::min(
                                nearestMeshDistance,
                                std::sqrt(dx * dx + dy * dy + dz * dz));
                        }

                        // The surface-safe near plane scales with altitude
                        // above the terrain, which is blind to imported
                        // meshes: a camera hovering metres from a building
                        // would clip it away. Never let the near plane
                        // reach into the nearest mesh.
                        if (std::isfinite(nearestMeshDistance))
                        {
                            const auto meshNear = static_cast<f32>(
                                std::max(nearestMeshDistance * 0.25, 0.02));
                            auto& cameraState = view->Camera();
                            cameraState.nearPlaneMeters = std::min(
                                cameraState.nearPlaneMeters, meshNear);
                            view->Lighting().nearPlaneMeters =
                                std::min(
                                    view->Lighting().nearPlaneMeters,
                                    meshNear);
                        }
                    }
                }
            }
        }

        const auto terrainRuntime =
            session.TerrainRuntime().
                Capture(
                    info.id);
        if (cpuTimingRecorder)
        {
            const auto now = std::chrono::steady_clock::now();
            cpuTimingRecorder(
                "early",
                std::chrono::duration<double, std::milli>(
                    now - composeStageStarted).count());
            composeStageStarted = now;
        }

        if (terrainRuntime.has_value() &&
            !session.TerrainRuntime().
                IsCurrent(
                    *terrainRuntime))
        {
            throw std::logic_error(
                "Studio terrain viewport runtime is stale for the current session generation.");
        }

        // Non-mesh geometry the mesh distance field also needs, so the sunlit
        // ground and nearby structures bounce light onto meshes: Visibility
        // Proxies (analytic boxes / spheres) and a terrain height patch.
        if (auto meshFound = staticMeshPresentations_.find(info.id);
            meshFound != staticMeshPresentations_.end())
        {
            auto& meshPresentation = meshFound->second;
            if (!meshPresentation.hasAnchor ||
                meshPresentation.instances.empty())
            {
                meshPresentation.sdfExtra.reset();
            }
            else
            {
                auto extra =
                    std::make_shared<mesh_render::SdfExtraGeometry>();
                const auto anchor = meshPresentation.anchorInFrame;

                if (const auto proxyFound =
                        visibilityProxyPresentations_.find(info.id);
                    !info.layers.bypassSdfProxies &&
                    proxyFound != visibilityProxyPresentations_.end() &&
                    proxyFound->second.scene.Frame() ==
                        view->Lighting().frame)
                {
                    // Centres come back relative to the anchor so float32
                    // keeps millimetres at planetary distances.
                    for (const auto& gpu :
                         proxyFound->second.scene.GpuPrimitives(anchor))
                    {
                        const bool box = gpu.centerType.w > 0.5F;
                        mesh_render::SdfProxyPrimitive primitive;
                        primitive.center = {
                            anchor.x + static_cast<f64>(gpu.centerType.x),
                            anchor.y + static_cast<f64>(gpu.centerType.y),
                            anchor.z + static_cast<f64>(gpu.centerType.z)};
                        primitive.box = box;
                        primitive.axisX = {
                            gpu.axisXExtent.x, gpu.axisXExtent.y,
                            gpu.axisXExtent.z};
                        primitive.axisY = {
                            gpu.axisYExtent.x, gpu.axisYExtent.y,
                            gpu.axisYExtent.z};
                        primitive.axisZ = {
                            gpu.axisZExtent.x, gpu.axisZExtent.y,
                            gpu.axisZExtent.z};
                        primitive.halfExtents = {
                            gpu.axisXExtent.w,
                            box ? gpu.axisYExtent.w : gpu.axisXExtent.w,
                            box ? gpu.axisZExtent.w : gpu.axisXExtent.w};
                        extra->primitives.push_back(primitive);
                    }
                }

                if (terrainRuntime.has_value() &&
                    !info.layers.bypassSdfTerrain)
                {
                    const auto& patchSource =
                        session.TerrainRuntime().TerrainSource(
                            *terrainRuntime);
                    const u64 sourceRevision = patchSource.Revision();
                    const f64 moved = math::Length(
                        anchor - meshPresentation.terrainPatchAnchor);
                    if (meshPresentation.terrainPatch == nullptr ||
                        meshPresentation.terrainPatchSourceRevision !=
                            sourceRevision ||
                        moved > 2.0)
                    {
                        constexpr u32 kCount = 193U;
                        constexpr f64 kCell = 1.0;
                        const auto bodyFromFrame =
                            math::Inverse(meshPresentation.targetFromBody);
                        const math::Double3 anchorBody =
                            math::TransformPoint(bodyFromFrame, anchor);
                        const f64 anchorRadius = math::Length(anchorBody);
                        if (anchorRadius > 1.0)
                        {
                            const math::Double3 upBody =
                                anchorBody * (1.0 / anchorRadius);
                            const math::Double3 helper =
                                std::abs(upBody.y) < 0.99
                                ? math::Double3{0.0, 1.0, 0.0}
                                : math::Double3{1.0, 0.0, 0.0};
                            math::Double3 eastBody =
                                math::Cross(helper, upBody);
                            eastBody = eastBody *
                                (1.0 / math::Length(eastBody));
                            const math::Double3 northBody =
                                math::Cross(upBody, eastBody);

                            const auto& rotation =
                                meshPresentation.targetFromBody.rotation;
                            const auto toFrame =
                                [&](const math::Double3& v)
                            {
                                return math::TransformVector(rotation, v);
                            };
                            const math::Double3 upFrame = toFrame(upBody);
                            const math::Double3 eastFrame =
                                toFrame(eastBody);
                            const math::Double3 northFrame =
                                toFrame(northBody);

                            auto patch = std::make_shared<
                                mesh_render::SdfTerrainPatch>();
                            patch->center = anchor;
                            patch->east = {
                                static_cast<f32>(eastFrame.x),
                                static_cast<f32>(eastFrame.y),
                                static_cast<f32>(eastFrame.z)};
                            patch->north = {
                                static_cast<f32>(northFrame.x),
                                static_cast<f32>(northFrame.y),
                                static_cast<f32>(northFrame.z)};
                            patch->up = {
                                static_cast<f32>(upFrame.x),
                                static_cast<f32>(upFrame.y),
                                static_cast<f32>(upFrame.z)};
                            patch->cellMeters = static_cast<f32>(kCell);
                            patch->count = kCount;
                            patch->revision =
                                ++meshPresentation.terrainPatchCounter;
                            patch->heights.resize(
                                static_cast<std::size_t>(kCount) * kCount);
                            const f64 half =
                                0.5 * static_cast<f64>(kCount - 1U);
                            for (u32 j = 0U; j < kCount; ++j)
                            {
                                for (u32 i = 0U; i < kCount; ++i)
                                {
                                    const math::Double3 planar =
                                        anchorBody +
                                        eastBody *
                                            ((static_cast<f64>(i) - half) *
                                             kCell) +
                                        northBody *
                                            ((static_cast<f64>(j) - half) *
                                             kCell);
                                    const f64 radius = math::Length(planar);
                                    const math::Double3 direction =
                                        planar * (1.0 / radius);
                                    const auto sample = patchSource.Sample({
                                        .unitDirection = direction,
                                        .footprintMeters = kCell,
                                        .planet =
                                            terrainRuntime->planet.id,
                                        .radialOffsetMeters = 0.0});
                                    const f64 elevation =
                                        std::isfinite(sample.elevationMeters)
                                        ? sample.elevationMeters
                                        : 0.0;
                                    const math::Double3 ground =
                                        direction *
                                        (terrainRuntime->planet
                                             .radiusMeters +
                                         elevation);
                                    patch->heights
                                        [static_cast<std::size_t>(j) *
                                             kCount + i] =
                                        static_cast<f32>(math::Dot(
                                            ground - anchorBody, upBody));
                                }
                            }
                            meshPresentation.terrainPatch = patch;
                            meshPresentation.terrainPatchAnchor = anchor;
                            meshPresentation.terrainPatchSourceRevision =
                                sourceRevision;
                        }
                    }
                    extra->terrain = meshPresentation.terrainPatch;
                }
                meshPresentation.sdfExtra = std::move(extra);
            }
        }

        const surface::TerrainSurfaceCapability* macroGlobeSurface = nullptr;

        if (logicalTarget->target.has_value() &&
            snapshot.hasWorld)
        {
            macroGlobeSurface =
                session.World().
                    Surfaces().
                    Registry().
                    FindTerrainSurface(
                        logicalTarget->target->body);
        }

        const bool hasMacroGlobe =
            macroGlobeSurface != nullptr &&
            macroGlobeSurface->terrain != nullptr;

        auto studioDirectLight =
            logicalTarget->target.has_value() &&
                    snapshot.hasWorld
                ? ResolveStudioDirectLight(
                      session,
                      logicalTarget->target->body,
                      atTime)
                : ResolvedStudioDirectLight{};

        // Cloud lab sun override: light the whole view (terrain, shadows, sky and
        // clouds) from the chosen sun at the lab cloud, so a terminator-lit cloud sits
        // on a terminator-lit landscape.
        if (const auto& lab = info.layers.cloudLab;
            lab.enabled && lab.overrideSun && studioDirectLight.direct.has_value())
        {
            if (const auto anchor = cloudLabAnchors_.find(info.id);
                anchor != cloudLabAnchors_.end() && anchor->second.valid)
            {
                const math::Double3 up = anchor->second.center;
                math::Double3 east = math::Cross(math::Double3{0.0, 1.0, 0.0}, up);
                if (math::Length(east) < 1.0e-6)
                {
                    east = math::Double3{1.0, 0.0, 0.0};
                }
                east = math::Normalize(east);
                const math::Double3 north = math::Cross(up, east);
                const f64 elevation =
                    static_cast<f64>(lab.sunElevationDegrees) * std::numbers::pi / 180.0;
                const f64 azimuth =
                    static_cast<f64>(lab.sunAzimuthDegrees) * std::numbers::pi / 180.0;
                const math::Double3 sun =
                    up * std::sin(elevation) +
                    (north * std::cos(azimuth) + east * std::sin(azimuth)) * std::cos(elevation);
                studioDirectLight.directionBody = {
                    static_cast<f32>(sun.x), static_cast<f32>(sun.y), static_cast<f32>(sun.z)};
            }
        }

        // ORBIT_REFRACT_SUN=1 (read once, default off): the sun is seen, lit and shadowed along its APPARENT direction,
        // lifted toward the observer's zenith by atmospheric refraction (about 29 arcminutes at the horizon, so a sunset
        // lasts a few minutes longer and the disc sits higher). directionBody is the single sun direction every consumer
        // below reads (atmosphere pass and sky, direct light, proxy shadow, glass, clouds), so refracting it once here
        // keeps them consistent. Uses the previous frame's atmosphere presentation for the parameters, so the first frame
        // is unrefracted; skipped under the cloud-lab sun override and for observers outside the atmosphere.
        if (RefractSunEnabled() && studioDirectLight.direct.has_value() &&
            !(info.layers.cloudLab.enabled && info.layers.cloudLab.overrideSun))
        {
            if (const auto presentation = atmospherePresentations_.find(info.id);
                presentation != atmospherePresentations_.end())
            {
                const auto& parameters = presentation->second.parameters;
                const math::Double3 position = view->Camera().localPositionMeters;
                const f64 radius = math::Length(position);
                const f64 altitude = radius - parameters.bottomRadiusMeters;
                if (radius > 1.0 && altitude >= 0.0 &&
                    altitude < parameters.topRadiusMeters - parameters.bottomRadiusMeters)
                {
                    const math::Double3 trueDirection = math::Normalize(math::Double3{
                        static_cast<f64>(studioDirectLight.directionBody.x),
                        static_cast<f64>(studioDirectLight.directionBody.y),
                        static_cast<f64>(studioDirectLight.directionBody.z)});
                    const auto apparent = celestial_atmosphere::RefractDirection(
                        celestial_atmosphere::RefractionTableFor(parameters),
                        position / radius,
                        trueDirection,
                        altitude);
                    if (apparent.visible)
                    {
                        studioDirectLight.directionBody = {
                            static_cast<f32>(apparent.direction.x),
                            static_cast<f32>(apparent.direction.y),
                            static_cast<f32>(apparent.direction.z)};
                    }
                }
            }
        }

        if (studioDirectLight.direct.has_value())
        {
            lightingDiagnostics_.insert_or_assign(
                info.id,
                StudioCelestialLightingDiagnostics{
                    .receiver =
                        studioDirectLight.direct->receiver,
                    .emitter =
                        studioDirectLight.direct->emitter,
                    .visibleFraction =
                        studioDirectLight.direct->visibleFraction,
                    .irradianceWattsPerSquareMeter =
                        studioDirectLight.direct->
                            irradianceWattsPerSquareMeter,
                    .contributingOccluders =
                        static_cast<u32>(
                            studioDirectLight.direct->
                                contributingOccluders.size())
                });
        }
        else
        {
            lightingDiagnostics_.erase(
                info.id);
        }

        const auto atmosphereBody =
            logicalTarget->target.has_value() &&
                    snapshot.hasWorld
                ? session.World().
                      Universe().
                      ObjectForBody(
                          logicalTarget->target->body)
                : std::nullopt;
        std::vector<
            world_model::ResolvedCloudLayer>
            resolvedCloudLayers;

        if (atmosphereBody.has_value())
        {
            resolvedCloudLayers =
                world_model::ResolveCloudLayers(
                    session.World().Objects(),
                    *atmosphereBody);
        }

        if (!resolvedCloudLayers.empty() &&
            logicalTarget->target.has_value())
        {
            std::vector<
                celestial_clouds::CloudLayerParameters>
                cloudParameters;
            cloudParameters.reserve(
                resolvedCloudLayers.size());

            bool requiresClimate = false;
            bool requiresExternal = false;

            for (const auto& layer :
                 resolvedCloudLayers)
            {
                cloudParameters.push_back(
                    layer.parameters);

                requiresClimate |=
                    layer.parameters.sourceModel ==
                    celestial_clouds::
                        CloudSourceModel::
                            ClimateProcedural;

                requiresExternal |=
                    layer.parameters.sourceModel ==
                        celestial_clouds::
                            CloudSourceModel::Authored ||
                    layer.parameters.sourceModel ==
                        celestial_clouds::
                            CloudSourceModel::Imported;
            }

            const terrain::TerrainSource*
                cloudClimateSource =
                    hasMacroGlobe
                        ? macroGlobeSurface->terrain.get()
                        : nullptr;

            if (requiresClimate &&
                cloudClimateSource == nullptr)
            {
                cloudPresentations_.erase(
                    info.id);
                cloudDiagnostics_.erase(
                    info.id);
            }
            else if (!requiresExternal)
            {
                const f64 referenceRadius =
                    shape.has_value()
                        ? universe::
                              ReferenceRadiusMeters(
                                  *shape)
                        : 1.0;

                // Seasons: the weather model's circulation cells follow the
                // latitude of the sub-stellar point.
                celestial_clouds::CloudFieldConfig cloudConfig{};
                if (studioDirectLight.direct.has_value())
                {
                    cloudConfig.subsolarLatitudeRadians = std::asin(
                        std::clamp(
                            static_cast<f64>(
                                studioDirectLight.directionBody.y),
                            -1.0,
                            1.0));
                }

                const u64 cloudFingerprint =
                    celestial_clouds::
                        CloudFieldFingerprint(
                            cloudClimateSource,
                            nullptr,
                            referenceRadius,
                            cloudParameters,
                            atTime,
                            cloudConfig);

                auto& cloudPresentation =
                    cloudPresentations_[info.id];

                // A field built in the background has landed: install it (the GPU upload stays on this thread).
                if (cloudPresentation.hasPending &&
                    cloudPresentation.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                {
                    cloudPresentation.hasPending = false;
                    try
                    {
                        cloudPresentation.field = std::make_unique<celestial_clouds::CloudFieldProduct>(cloudPresentation.pending.get());
                        cloudPresentation.gpu = std::make_unique<celestial_clouds::GpuCloudFieldProduct>(*device_, *cloudPresentation.field);
                        cloudPresentation.body = cloudPresentation.pendingBody;
                        cloudPresentation.fingerprint = cloudPresentation.pendingFingerprint;
                    }
                    catch (const std::exception& exception)
                    {
                        log::Warning(std::string("Cloud field build failed: ") + exception.what());
                    }
                    static_cast<void>(
                        completeCelestialGrant(
                            info.id,
                            cloudPresentation.pendingBody,
                            celestial_scheduler::WorkKind::CloudField,
                            cloudPresentation.pendingFingerprint));
                }

                const bool cloudNeedsBuild =
                    cloudPresentation.field ==
                        nullptr ||
                    cloudPresentation.body !=
                        logicalTarget->
                            target->body ||
                    cloudPresentation.fingerprint !=
                        cloudFingerprint;

                if (cloudNeedsBuild &&
                    !cloudPresentation.hasPending &&
                    acquireCelestialGrant(
                        info.id,
                        logicalTarget->
                            target->body,
                        celestial_scheduler::
                            WorkKind::CloudField,
                        cloudFingerprint,
                        celestial_scheduler::
                            WorkBackend::Gpu,
                        3U,
                        70,
                        true))
                {
                  if (cloudPresentation.field != nullptr &&
                      cloudPresentation.body == logicalTarget->target->body)
                  {
                    // The same body with a newer time: build it on another thread. The task keeps the terrain source alive (concurrent
                    // Sample calls are part of TerrainSource's contract: the streaming workers do the same).
                    std::shared_ptr<const terrain::TerrainSource> climateHold =
                        macroGlobeSurface != nullptr ? macroGlobeSurface->terrain : nullptr;
                    cloudPresentation.pending = std::async(
                        std::launch::async,
                        [climateHold, referenceRadius, parameters = cloudParameters, atTime, cloudConfig]() {
                            return celestial_clouds::BuildCloudField(climateHold.get(), nullptr, referenceRadius, parameters, atTime, cloudConfig);
                        });
                    cloudPresentation.hasPending = true;
                    cloudPresentation.pendingFingerprint = cloudFingerprint;
                    cloudPresentation.pendingBody = logicalTarget->target->body;
                  }
                  else
                  {
                    cloudPresentation.field =
                        std::make_unique<
                            celestial_clouds::
                                CloudFieldProduct>(
                                    celestial_clouds::
                                        BuildCloudField(
                                            cloudClimateSource,
                                            nullptr,
                                            referenceRadius,
                                            cloudParameters,
                                            atTime,
                                            cloudConfig));

                    cloudPresentation.gpu =
                        std::make_unique<
                            celestial_clouds::
                                GpuCloudFieldProduct>(
                                    *device_,
                                    *cloudPresentation.
                                        field);

                    cloudPresentation.body =
                        logicalTarget->
                            target->body;
                    cloudPresentation.fingerprint =
                        cloudFingerprint;

                    static_cast<void>(
                        completeCelestialGrant(
                            info.id,
                            logicalTarget->
                                target->body,
                            celestial_scheduler::
                                WorkKind::CloudField,
                            cloudFingerprint));
                  }
                }

                const bool cloudCurrent =
                    cloudPresentation.field !=
                        nullptr &&
                    cloudPresentation.body ==
                        logicalTarget->
                            target->body &&
                    cloudPresentation.fingerprint ==
                        cloudFingerprint;

                if (cloudCurrent)
                {
                    f64 coverageSum = 0.0;
                    f64 opticalSum = 0.0;
                    u64 sampleCount = 0U;

                    for (const auto& layer :
                         cloudPresentation.field->layers)
                    {
                        for (const auto& texel :
                             layer.texels)
                        {
                            coverageSum +=
                                texel.coverage;
                            opticalSum +=
                                texel.opticalDepth;
                            ++sampleCount;
                        }
                    }

                    cloudDiagnostics_.insert_or_assign(
                        info.id,
                        StudioCloudDiagnostics{
                            .body =
                                logicalTarget->
                                    target->body,
                            .fingerprint =
                                cloudPresentation.
                                    fingerprint,
                            .climateRevision =
                                cloudPresentation.
                                    field->
                                    climateRevision,
                            .timeBucket =
                                cloudPresentation.
                                    field->
                                    timeBucket,
                            .layerCount =
                                static_cast<u32>(
                                    cloudPresentation.
                                        field->
                                        layers.size()),
                            .meanCoverage =
                                sampleCount > 0U
                                    ? coverageSum /
                                          static_cast<f64>(
                                              sampleCount)
                                    : 0.0,
                            .meanOpticalDepth =
                                sampleCount > 0U
                                    ? opticalSum /
                                          static_cast<f64>(
                                              sampleCount)
                                    : 0.0,
                            .gpuResident =
                                cloudPresentation.gpu !=
                                nullptr
                        });
                }
                else
                {
                    cloudDiagnostics_.erase(
                        info.id);
                }
            }
            else
            {
                // External authored/imported source selection is semantic
                // authority; Studio waits for the selected source object's
                // coverage adapter rather than silently substituting
                // procedural weather.
                cloudPresentations_.erase(
                    info.id);
                cloudDiagnostics_.erase(
                    info.id);
            }
        }
        else
        {
            cloudPresentations_.erase(
                info.id);
            cloudDiagnostics_.erase(
                info.id);
        }

        std::optional<
            world_model::ResolvedOceanBody>
            resolvedOceanForView;

        if (atmosphereBody.has_value())
        {
            resolvedOceanForView =
                world_model::
                    ResolveOceanBody(
                        session.World().
                            Objects(),
                        *atmosphereBody);
        }

        if (!info.layers.ocean)
        {
            resolvedOceanForView.reset();
        }

        std::optional<
            world_model::ResolvedRingSystem>
            resolvedRingSystemForView;

        if (atmosphereBody.has_value())
        {
            resolvedRingSystemForView =
                world_model::
                    ResolveRingSystem(
                        session.World().
                            Objects(),
                        *atmosphereBody);
        }

        celestial_rings::GpuRingMeshProduct*
            activeRingMesh = nullptr;
        bool activeRingNear = false;

        if (resolvedRingSystemForView.has_value() &&
            logicalTarget->target.has_value() &&
            shape.has_value() &&
            !resolvedRingSystemForView->
                parameters.bands.empty())
        {
            const f64 referenceRadius =
                ReferenceRadiusForShape(
                    *shape);

            const f64 outerRadius =
                resolvedRingSystemForView->
                    parameters.bands.back().
                    outerRadiusMeters;

            const f64 cameraDistance =
                math::Length(
                    view->Camera().
                        localPositionMeters);

            const f64 projectedRingRadiusPixels =
                cameraDistance > outerRadius
                    ? std::asin(
                          std::clamp(
                              outerRadius /
                                  cameraDistance,
                              0.0,
                              1.0)) /
                          std::max(
                              static_cast<f64>(
                                  view->Camera().
                                      verticalFovRadians),
                              1.0e-6) *
                          static_cast<f64>(
                              std::max(
                                  view->Height(),
                                  1U))
                    : static_cast<f64>(
                          std::max(
                              view->Height(),
                              1U)) *
                          0.5;

            activeRingNear =
                projectedRingRadiusPixels >=
                180.0;

            auto& rings =
                ringPresentations_[info.id];

            const u64 semanticFingerprint =
                resolvedRingSystemForView->
                    parameters.fingerprint;

            const bool ringsNeedBuild =
                rings.nearMesh == nullptr ||
                rings.farMesh == nullptr ||
                rings.body !=
                    logicalTarget->
                        target->body ||
                rings.fingerprint !=
                    semanticFingerprint ||
                rings.referenceRadiusMeters !=
                    referenceRadius;

            if (ringsNeedBuild &&
                acquireCelestialGrant(
                    info.id,
                    logicalTarget->
                        target->body,
                    celestial_scheduler::
                        WorkKind::RingPresentation,
                    semanticFingerprint,
                    celestial_scheduler::
                        WorkBackend::Gpu,
                    3U,
                    65,
                    true))
            {
                const auto nearCpu =
                    celestial_rings::
                        BuildRingMesh(
                            resolvedRingSystemForView->
                                parameters,
                            referenceRadius,
                            256U);

                const auto farCpu =
                    celestial_rings::
                        BuildRingMesh(
                            resolvedRingSystemForView->
                                parameters,
                            referenceRadius,
                            64U);

                rings.nearMesh =
                    std::make_unique<
                        celestial_rings::
                            GpuRingMeshProduct>(
                                *device_,
                                nearCpu);
                rings.farMesh =
                    std::make_unique<
                        celestial_rings::
                            GpuRingMeshProduct>(
                                *device_,
                                farCpu);
                rings.farProfile =
                    celestial_rings::
                        BuildFarRingProfile(
                            resolvedRingSystemForView->
                                parameters,
                            referenceRadius,
                            256U);
                rings.body =
                    logicalTarget->
                        target->body;
                rings.fingerprint =
                    semanticFingerprint;
                rings.referenceRadiusMeters =
                    referenceRadius;

                static_cast<void>(
                    completeCelestialGrant(
                        info.id,
                        logicalTarget->
                            target->body,
                        celestial_scheduler::
                            WorkKind::RingPresentation,
                        semanticFingerprint));
            }

            const bool ringsCurrent =
                rings.nearMesh != nullptr &&
                rings.farMesh != nullptr &&
                rings.body ==
                    logicalTarget->
                        target->body &&
                rings.fingerprint ==
                    semanticFingerprint &&
                rings.referenceRadiusMeters ==
                    referenceRadius;

            if (ringsCurrent)
            {
                activeRingMesh =
                    activeRingNear
                        ? rings.nearMesh.get()
                        : rings.farMesh.get();

                ringDiagnostics_.insert_or_assign(
                    info.id,
                    StudioRingDiagnostics{
                        .body =
                            logicalTarget->
                                target->body,
                        .fingerprint =
                            semanticFingerprint,
                        .bandCount =
                            static_cast<u32>(
                                resolvedRingSystemForView->
                                    parameters.bands.size()),
                        .innerRadiusMeters =
                            resolvedRingSystemForView->
                                parameters.bands.front().
                                innerRadiusMeters,
                        .outerRadiusMeters =
                            outerRadius,
                        .projectedOuterRadiusPixels =
                            projectedRingRadiusPixels,
                        .nearRepresentation =
                            activeRingNear,
                        .angularSegments =
                            activeRingNear
                                ? 256U
                                : 64U,
                        .farProfileSamples =
                            rings.farProfile.
                                radialSamples
                    });
            }
            else
            {
                ringDiagnostics_.erase(
                    info.id);
            }
        }
        else
        {
            ringPresentations_.erase(
                info.id);
            ringDiagnostics_.erase(
                info.id);
        }

        std::optional<
            world_model::ResolvedMagnetosphere>
            resolvedMagnetosphereForView;

        if (atmosphereBody.has_value() &&
            shape.has_value())
        {
            resolvedMagnetosphereForView =
                world_model::
                    ResolveMagnetosphere(
                        session.World().
                            Objects(),
                        *atmosphereBody,
                        ReferenceRadiusForShape(
                            *shape));
        }

        celestial_magnetosphere_render::
            GpuAuroraMeshProduct*
            activeAuroraMesh = nullptr;
        bool activeAuroraNear = false;

        if (resolvedMagnetosphereForView.has_value() &&
            logicalTarget->target.has_value() &&
            shape.has_value())
        {
            const f64 referenceRadius =
                ReferenceRadiusForShape(
                    *shape);
            const f64 outerRadius =
                referenceRadius +
                resolvedMagnetosphereForView->
                    parameters.
                    auroralMaximumAltitudeMeters;
            const f64 cameraDistance =
                math::Length(
                    view->Camera().
                        localPositionMeters);

            const f64 projectedAuroraRadiusPixels =
                cameraDistance > outerRadius
                    ? std::asin(
                          std::clamp(
                              outerRadius /
                                  cameraDistance,
                              0.0,
                              1.0)) /
                          std::max(
                              static_cast<f64>(
                                  view->Camera().
                                      verticalFovRadians),
                              1.0e-6) *
                          static_cast<f64>(
                              std::max(
                                  view->Height(),
                                  1U))
                    : static_cast<f64>(
                          std::max(
                              view->Height(),
                              1U)) *
                          0.5;

            activeAuroraNear =
                projectedAuroraRadiusPixels >=
                160.0;

            auto& presentation =
                magnetospherePresentations_[
                    info.id];

            const bool auroraNeedsBuild =
                presentation.nearAurora ==
                    nullptr ||
                presentation.farAurora ==
                    nullptr ||
                presentation.body !=
                    logicalTarget->
                        target->body ||
                presentation.fingerprint !=
                    resolvedMagnetosphereForView->
                        fingerprint ||
                presentation.referenceRadiusMeters !=
                    referenceRadius;

            if (auroraNeedsBuild &&
                acquireCelestialGrant(
                    info.id,
                    logicalTarget->
                        target->body,
                    celestial_scheduler::
                        WorkKind::AuroraPresentation,
                    resolvedMagnetosphereForView->
                        fingerprint,
                    celestial_scheduler::
                        WorkBackend::Gpu,
                    3U,
                    60,
                    true))
            {
                const auto nearCpu =
                    celestial_magnetosphere::
                        BuildAuroraCurtainMesh(
                            resolvedMagnetosphereForView->
                                parameters,
                            referenceRadius,
                            256U);

                const auto farCpu =
                    celestial_magnetosphere::
                        BuildAuroraCurtainMesh(
                            resolvedMagnetosphereForView->
                                parameters,
                            referenceRadius,
                            64U);

                presentation.nearAurora =
                    std::make_unique<
                        celestial_magnetosphere_render::
                            GpuAuroraMeshProduct>(
                                *device_,
                                nearCpu);
                presentation.farAurora =
                    std::make_unique<
                        celestial_magnetosphere_render::
                            GpuAuroraMeshProduct>(
                                *device_,
                                farCpu);
                presentation.body =
                    logicalTarget->
                        target->body;
                presentation.fingerprint =
                    resolvedMagnetosphereForView->
                        fingerprint;
                presentation.referenceRadiusMeters =
                    referenceRadius;

                static_cast<void>(
                    completeCelestialGrant(
                        info.id,
                        logicalTarget->
                            target->body,
                        celestial_scheduler::
                            WorkKind::AuroraPresentation,
                        resolvedMagnetosphereForView->
                            fingerprint));
            }

            const bool auroraCurrent =
                presentation.nearAurora !=
                    nullptr &&
                presentation.farAurora !=
                    nullptr &&
                presentation.body ==
                    logicalTarget->
                        target->body &&
                presentation.fingerprint ==
                    resolvedMagnetosphereForView->
                        fingerprint &&
                presentation.referenceRadiusMeters ==
                    referenceRadius;

            if (auroraCurrent)
            {
                activeAuroraMesh =
                    activeAuroraNear
                        ? presentation.
                              nearAurora.get()
                        : presentation.
                              farAurora.get();

                const auto product =
                    celestial_magnetosphere::
                        BuildMagnetosphereProduct(
                            resolvedMagnetosphereForView->
                                parameters,
                            referenceRadius,
                            {.ovalSamples =
                                 activeAuroraNear
                                     ? 256U
                                     : 64U});

                magnetosphereDiagnostics_.
                    insert_or_assign(
                        info.id,
                        StudioMagnetosphereDiagnostics{
                            .body =
                                logicalTarget->
                                    target->body,
                            .fingerprint =
                                resolvedMagnetosphereForView->
                                    fingerprint,
                            .subsolarStandoffMeters =
                                product.
                                    subsolarStandoffMeters,
                            .tailExtentMeters =
                                product.
                                    tailExtentMeters,
                            .auroralCenterLatitudeDegrees =
                                product.
                                    auroralCenterLatitudeDegrees,
                            .auroralMinimumAltitudeMeters =
                                resolvedMagnetosphereForView->
                                    parameters.
                                    auroralMinimumAltitudeMeters,
                            .auroralMaximumAltitudeMeters =
                                resolvedMagnetosphereForView->
                                    parameters.
                                    auroralMaximumAltitudeMeters,
                            .projectedAuroraRadiusPixels =
                                projectedAuroraRadiusPixels,
                            .activity =
                                resolvedMagnetosphereForView->
                                    parameters.activity,
                            .nearRepresentation =
                                activeAuroraNear,
                            .angularSegments =
                                activeAuroraNear
                                    ? 256U
                                    : 64U
                        });
            }
            else
            {
                magnetosphereDiagnostics_.erase(
                    info.id);
            }
        }
        else
        {
            magnetospherePresentations_.erase(
                info.id);
            magnetosphereDiagnostics_.erase(
                info.id);
        }

        std::optional<
            world_model::ResolvedAtmosphereBody>
            resolvedAtmosphere;

        if (atmosphereBody.has_value())
        {
            resolvedAtmosphere =
                world_model::
                    ResolveAtmosphereBody(
                        session.World().
                            Objects(),
                        *atmosphereBody);
        }

        if (resolvedAtmosphere.has_value() &&
            logicalTarget->target.has_value())
        {
            const celestial_atmosphere::
                AtmosphereLutConfig
                atmosphereConfig{};

            auto& presentation =
                atmospherePresentations_[
                    info.id];

            const u64 staticFingerprint =
                celestial_atmosphere::
                    AtmosphereFingerprint(
                        resolvedAtmosphere->
                            parameters,
                        atmosphereConfig);

            const bool staticChanged =
                presentation.staticLuts ==
                    nullptr ||
                presentation.body !=
                    logicalTarget->
                        target->body ||
                presentation.staticFingerprint !=
                    staticFingerprint;

            if (staticChanged &&
                acquireCelestialGrant(
                    info.id,
                    logicalTarget->
                        target->body,
                    celestial_scheduler::
                        WorkKind::AtmosphereLut,
                    staticFingerprint,
                    celestial_scheduler::
                        WorkBackend::Gpu,
                    4U,
                    90,
                    true))
            {
                presentation.staticLuts =
                    std::make_unique<
                        celestial_atmosphere::
                            AtmosphereStaticLuts>(
                                celestial_atmosphere::
                                    BuildStaticLuts(
                                        resolvedAtmosphere->
                                            parameters,
                                        atmosphereConfig));

                presentation.body =
                    logicalTarget->
                        target->body;
                presentation.staticFingerprint =
                    staticFingerprint;
                presentation.parameters =
                    resolvedAtmosphere->
                        parameters;
                presentation.skyView.reset();
                presentation.gpu.reset();
                presentation.skyFingerprint = 0U;

                static_cast<void>(
                    completeCelestialGrant(
                        info.id,
                        logicalTarget->
                            target->body,
                        celestial_scheduler::
                            WorkKind::AtmosphereLut,
                        staticFingerprint));
            }

            const bool atmosphereCurrent =
                presentation.staticLuts !=
                    nullptr &&
                presentation.body ==
                    logicalTarget->
                        target->body &&
                presentation.staticFingerprint ==
                    staticFingerprint;

            if (atmosphereCurrent)
            {
            const f64 rawObserverRadius =
                math::Length(
                    view->Camera().
                        localPositionMeters);

            const f64 observerRadius =
                QuantizedSkyObserverRadius(
                    std::max(
                        rawObserverRadius,
                        resolvedAtmosphere->
                            parameters.
                            bottomRadiusMeters +
                            1.0e-3),
                    resolvedAtmosphere->
                        parameters.
                        bottomRadiusMeters);

            const f64 irradiance =
                studioDirectLight.
                        direct.has_value()
                    ? studioDirectLight.
                          direct->
                          irradianceWattsPerSquareMeter
                    : 0.0;

            const celestial_atmosphere::
                SkyViewInput
                skyInput{
                    .observerRadiusMeters =
                        observerRadius,
                    // The sky table lives in a sky frame (+Z = this observer's
                    // zenith, sun at azimuth 0), so the sun goes in as that
                    // frame's direction, not as a body-fixed one.
                    .sunDirectionBody =
                        celestial_atmosphere::SkyFrameSunDirection(
                            view->Camera().localPositionMeters,
                            {
                                studioDirectLight.
                                    directionBody.x,
                                studioDirectLight.
                                    directionBody.y,
                                studioDirectLight.
                                    directionBody.z
                            }),
                    .incidentIrradianceWattsPerSquareMeter = {
                        irradiance,
                        irradiance,
                        irradiance
                    }
                };

            const u64 skyFingerprint =
                celestial_atmosphere::
                    AtmosphereSkyFingerprint(
                        resolvedAtmosphere->
                            parameters,
                        presentation.
                            staticFingerprint,
                        skyInput,
                        atmosphereConfig);

            const bool skyNeedsBuild =
                presentation.skyView ==
                    nullptr ||
                presentation.skyFingerprint !=
                    skyFingerprint;

            if (skyNeedsBuild &&
                acquireCelestialGrant(
                    info.id,
                    logicalTarget->
                        target->body,
                    celestial_scheduler::
                        WorkKind::AtmosphereSky,
                    skyFingerprint,
                    celestial_scheduler::
                        WorkBackend::Gpu,
                    2U,
                    95,
                    true))
            {
                presentation.skyView =
                    std::make_unique<
                        celestial_atmosphere::
                            AtmosphereSkyView>(
                                celestial_atmosphere::
                                    BuildSkyView(
                                        resolvedAtmosphere->
                                            parameters,
                                        *presentation.
                                            staticLuts,
                                        skyInput,
                                        atmosphereConfig));

                presentation.skyFingerprint =
                    skyFingerprint;

                if (presentation.gpu ==
                    nullptr)
                {
                    presentation.gpu =
                        std::make_unique<
                            celestial_atmosphere::
                                GpuAtmosphereLuts>(
                                    *device_,
                                    *presentation.
                                        staticLuts,
                                    *presentation.
                                        skyView);
                }
                else
                {
                    presentation.gpu->
                        ReplaceSkyView(
                            *presentation.
                                skyView);
                }

                static_cast<void>(
                    completeCelestialGrant(
                        info.id,
                        logicalTarget->
                            target->body,
                        celestial_scheduler::
                            WorkKind::AtmosphereSky,
                        skyFingerprint));
            }

            const bool skyCurrent =
                presentation.skyView !=
                    nullptr &&
                presentation.skyFingerprint ==
                    skyFingerprint;

            if (skyCurrent)
            {
            atmosphereDiagnostics_.
                insert_or_assign(
                    info.id,
                    StudioAtmosphereDiagnostics{
                        .body =
                            logicalTarget->
                                target->body,
                        .staticFingerprint =
                            presentation.
                                staticFingerprint,
                        .skyFingerprint =
                            presentation.
                                skyFingerprint,
                        .observerRadiusMeters =
                            rawObserverRadius,
                        .observerAltitudeMeters =
                            rawObserverRadius -
                            resolvedAtmosphere->
                                parameters.
                                bottomRadiusMeters,
                        .directIrradianceWattsPerSquareMeter =
                            irradiance,
                        .transmittanceWidth =
                            presentation.
                                staticLuts->
                                transmittance.width,
                        .transmittanceHeight =
                            presentation.
                                staticLuts->
                                transmittance.height,
                        .multiScatteringWidth =
                            presentation.
                                staticLuts->
                                multiScattering.width,
                        .multiScatteringHeight =
                            presentation.
                                staticLuts->
                                multiScattering.height,
                        .skyViewWidth =
                            presentation.
                                skyView->
                                skyView.width,
                        .skyViewHeight =
                            presentation.
                                skyView->
                                skyView.height
                    });
            }
            else
            {
                atmosphereDiagnostics_.erase(
                    info.id);
            }
            }
            else
            {
                atmosphereDiagnostics_.erase(
                    info.id);
            }
        }
        else
        {
            atmospherePresentations_.erase(
                info.id);
            atmosphereDiagnostics_.erase(
                info.id);
        }

        if (const auto atmosphereFound =
                atmospherePresentations_.find(
                    info.id);
            atmosphereFound !=
                    atmospherePresentations_.end() &&
                atmosphereFound->second.gpu !=
                    nullptr)
        {
            auto* atmosphereGpu =
                atmosphereFound->
                    second.gpu.get();

            graph.AddPass(
                prefix +
                    ".AtmosphereLutUpload",
                {},
                [atmosphereGpu](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    atmosphereGpu->
                        EnsureUploaded(
                            commands);
                });
        }

        const auto liveDebugPage =
            views.LiveDebugPage(info.id);
        const bool hasDebugField =
            liveDebugPage != nullptr &&
            liveDebugPage->Has(
                info.debugField);

        const auto presentation =
            SelectStudioViewportPresentation(
                logicalTarget->mode,
                shape.has_value(),
                terrainRuntime.has_value(),
                hasMacroGlobe,
                liveDebugPage != nullptr,
                hasDebugField);

        const u32 width = view->Width();
        const u32 height = view->Height();
        auto* color = &view->Color();
        const auto systemBodies =
            snapshot.hasWorld && logicalTarget->target.has_value() &&
                    logicalTarget->mode == studio_session::ViewportMode::Perspective
                ? ResolveSystemBodyDraws(
                      session, content_, logicalTarget->target->body,
                      view->Camera(), atTime, width, height)
                : std::vector<celestial_far_render::FarBodyDraw>{};
        auto backgroundBodies = std::make_shared<std::vector<
            celestial_far_render::FarBodyDraw>>();
        auto foregroundBodies = std::make_shared<std::vector<
            celestial_far_render::FarBodyDraw>>();
        const f64 activeDistance = math::Length(
            view->Camera().localPositionMeters);
        // Draw far bodies before the active presentation and closer bodies
        // after it. Stable far-to-near order also handles mutual transits.
        for (const auto& draw : systemBodies)
        {
            (math::Length(draw.camera.localPositionMeters) < activeDistance
                 ? *foregroundBodies
                 : *backgroundBodies).push_back(draw);
        }
        const auto drawBackgroundBodies =
            [this, color, width, height, backgroundBodies](
                rhi::CommandList& commands)
            {
                for (const auto& draw : *backgroundBodies)
                {
                    farBodyRenderer_.Draw(
                        commands, *color, width, height, draw);
                }
            };
        const auto drawForegroundBodies =
            [this, color, width, height, foregroundBodies](
                rhi::CommandList& commands)
            {
                for (const auto& draw : *foregroundBodies)
                {
                    farBodyRenderer_.Draw(
                        commands, *color, width, height, draw);
                }
            };

        // Only the production-terrain presentation clears depth itself.
        // Every other presentation must start from an empty depth buffer, or
        // depth-driven passes (GI fallback, reflections, atmosphere, particles)
        // see phantom surfaces left behind by the previously viewed body.
        if (presentation !=
            StudioViewportPresentation::ProductionTerrain)
        {
            auto* staleDepth = &view->Depth();

            graph.AddPass(
                prefix + ".ClearDepth",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::RenderTarget,
                        .access =
                            render_graph::Access::Write
                    },
                    {
                        .texture = targets.depth,
                        .state =
                            rhi::ResourceState::DepthWrite,
                        .access =
                            render_graph::Access::Write
                    }
                },
                [color, staleDepth](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    // Clears are deferred to the next rendering scope that
                    // binds the attachment; bind depth here so the clear
                    // actually executes this frame.
                    commands.ClearDepthTarget(
                        *staleDepth,
                        0.0F);
                    commands.SetRenderTargets(
                        *color,
                        *staleDepth);
                });
        }

        // Near-field representation weight and sea level, recorded by the
        // production-terrain case for the water pass added after lighting.
        f64 nearFieldWaterWeight = 0.0;
        f32 nearFieldSeaLevelMeters = 0.0F;

        switch (presentation)
        {
        case StudioViewportPresentation::ProductionTerrain:
        {
            StudioScenePassContext terrainContext{
                graph, views, session, view, info, targets, prefix, terrainRuntime,
                nearFieldWaterWeight, nearFieldSeaLevelMeters, hasMacroGlobe,
                studioDirectLight, cpuTimingRecorder, composeStageStarted,
                resolvedOceanForView, frameIndex, drawBackgroundBodies,
                recordComposeStage, macroGlobeSurface, shape,
                acquireCelestialGrant, completeCelestialGrant};
            ComposeTerrainPass(terrainContext);
            break;
        }
        case StudioViewportPresentation::TerrainDebug:
        {
            transitionDiagnostics_.erase(info.id);
            clipmapPlanStats_.erase(info.id);

            macroGlobePresentations_.erase(
                info.id);

            terrainPresentations_.erase(
                info.id);

            if (device_ == nullptr ||
                liveDebugPage == nullptr)
            {
                throw std::logic_error(
                    "Studio terrain-debug presentation lost its device or live page.");
            }

            auto& debug =
                debugPresentations_[info.id];

            if (debug.texture == nullptr ||
                debug.texture->Width() !=
                    liveDebugPage->Width() ||
                debug.texture->Height() !=
                    liveDebugPage->Height())
            {
                debug.texture =
                    std::make_unique<
                        terrain_debug::TerrainDebugTexture>(
                            *device_,
                            liveDebugPage->Width(),
                            liveDebugPage->Height());
                debug.source.reset();
            }

            const auto seams =
                terrain_debug::InspectTerrainDebugSeams(
                    *liveDebugPage,
                    info.debugField,
                    session.TerrainDebugPages());

            const u64 seamFingerprint =
                terrain_debug::
                    TerrainDebugSeamOverlayFingerprint(
                        seams);

            const bool needsUpload =
                debug.source != liveDebugPage ||
                debug.field != info.debugField ||
                debug.seamFingerprint !=
                    seamFingerprint ||
                !debug.texture->HasContent();

            auto* debugTexture =
                debug.texture.get();
            const auto field =
                info.debugField;

            graph.AddPass(
                prefix + ".TerrainDebug",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 debugTexture,
                 liveDebugPage,
                 field,
                 seams,
                 needsUpload](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    if (needsUpload)
                    {
                        debugTexture->Upload(
                            commands,
                            liveDebugPage->View(field),
                            seams);
                    }

                    debugComposite_.Draw(
                        commands,
                        debugTexture->Texture(),
                        *color,
                        width,
                        height);
                });

            debug.source = liveDebugPage;
            debug.field = field;
            debug.seamFingerprint =
                seamFingerprint;
            break;
        }

        case StudioViewportPresentation::TerrainDebugUnavailable:
        {
            transitionDiagnostics_.erase(info.id);
            clipmapPlanStats_.erase(info.id);

            macroGlobePresentations_.erase(
                info.id);
            terrainPresentations_.erase(
                info.id);

            graph.AddPass(
                prefix + ".TerrainDebugUnavailable",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [color](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.055F,
                            .green = 0.018F,
                            .blue = 0.024F,
                            .alpha = 1.0F
                        });
                });
            break;
        }

        case StudioViewportPresentation::MacroGlobe:
        {
            transitionDiagnostics_.erase(info.id);
            clipmapPlanStats_.erase(info.id);
            terrainPresentations_.erase(
                info.id);

            if (device_ == nullptr ||
                macroGlobeSurface == nullptr ||
                macroGlobeSurface->terrain == nullptr ||
                !logicalTarget->target.has_value() ||
                !shape.has_value())
            {
                throw std::logic_error(
                    "Studio macro-globe presentation lost its terrain authority, device, shape, or target body.");
            }

            const f64 globeProjectedRadiusPixels =
                [&]()
                {
                    const auto globeCamera =
                        view->Camera();
                    const f64 radius =
                        ReferenceRadiusForShape(*shape);
                    const f64 distance =
                        std::max(
                            math::Length(
                                globeCamera.localPositionMeters),
                            radius * 1.000001);
                    const f64 angularRadius =
                        std::asin(
                            std::clamp(
                                radius / distance,
                                0.0,
                                1.0));
                    return std::tan(angularRadius) /
                        std::max(
                            std::tan(
                                static_cast<f64>(
                                    globeCamera.verticalFovRadians) *
                                0.5),
                            1.0e-6) *
                        static_cast<f64>(height) *
                        0.5;
                }();

            auto* globe =
                EnsureMacroGlobePresentation(
                    info.id,
                    session,
                    logicalTarget->target->body,
                    *shape,
                    *macroGlobeSurface->terrain,
                    globeProjectedRadiusPixels,
                    [&](const u64 revision)
                    {
                        return acquireCelestialGrant(
                            info.id,
                            logicalTarget->target->body,
                            celestial_scheduler::
                                WorkKind::FarImpostor,
                            revision,
                            celestial_scheduler::
                                WorkBackend::Gpu,
                            5U,
                            75,
                            true);
                    },
                    [&](const u64 revision)
                    {
                        static_cast<void>(
                            completeCelestialGrant(
                                info.id,
                                logicalTarget->target->body,
                                celestial_scheduler::
                                    WorkKind::FarImpostor,
                                revision));
                    });

            const auto camera =
                view->Camera();
            auto* macroSurfaceBaseRoughness =
                &view->SurfaceBaseRoughness();
            auto* macroSurfaceNormalMetallic =
                &view->SurfaceNormalMetallic();
            auto* macroSurfaceEmissionClass =
                &view->SurfaceEmissionClass();

            graph.AddPass(
                prefix + ".MacroGlobe",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    },
                    {
                        .texture = targets.surfaceBaseRoughness,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    },
                    {
                        .texture = targets.surfaceNormalMetallic,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    },
                    {
                        .texture = targets.surfaceEmissionClass,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 color,
                 macroSurfaceBaseRoughness,
                 macroSurfaceNormalMetallic,
                 macroSurfaceEmissionClass,
                 width,
                 height,
                 globe,
                 camera,
                 studioDirectLight,
                 resolvedOceanForView,
                 drawBackgroundBodies,
                 globeLodBias = info.layers.lodBiasStops,
                 macroGlobeLayer = (info.layers.macroGlobe && !info.layers.fullClipmap)](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.0F,
                            .green = 0.0F,
                            .blue = 0.0F,
                            .alpha = 1.0F
                        });
                    commands.ClearColorTarget(
                        *macroSurfaceBaseRoughness,
                        {0.0F, 0.0F, 0.0F, 1.0F});
                    commands.ClearColorTarget(
                        *macroSurfaceNormalMetallic,
                        {0.0F, 1.0F, 0.0F, 0.0F});
                    commands.ClearColorTarget(
                        *macroSurfaceEmissionClass,
                        {0.0F, 0.0F, 0.0F, 0.0F});

                    drawBackgroundBodies(commands);

                    if (globe == nullptr || !macroGlobeLayer)
                    {
                        return;
                    }

                    macroGlobeRenderer_.DrawSurface(
                        commands,
                        *color,
                        *macroSurfaceBaseRoughness,
                        *macroSurfaceNormalMetallic,
                        *macroSurfaceEmissionClass,
                        width,
                        height,
                        *globe,
                        camera,
                        1.0F,
                        celestial_globe::
                            MacroGlobeLighting{
                                .directionBody =
                                    studioDirectLight.
                                        directionBody,
                                .irradianceScale =
                                    studioDirectLight.
                                        irradianceScale,
                                .oceanRefractiveIndex =
                                    static_cast<f32>(
                                        resolvedOceanForView.has_value()
                                            ? resolvedOceanForView->optical.refractiveIndex
                                            : 1.333),
                                .oceanRoughness =
                                    static_cast<f32>(
                                        resolvedOceanForView.has_value()
                                            ? resolvedOceanForView->optical.orbitalRoughness
                                            : 0.12),
                                .oceanGlintStrength =
                                    static_cast<f32>(
                                        resolvedOceanForView.has_value()
                                            ? resolvedOceanForView->optical.glintStrength
                                            : 1.0),
                                .oceanEnabled =
                                    resolvedOceanForView.has_value(),
                                .lodBiasStops = globeLodBias
                            });
                });

            break;
        }

        case StudioViewportPresentation::BodyPreview:
        {
            StudioScenePassContext sceneContext{
                graph, views, session, view, info, targets, prefix, terrainRuntime,
                nearFieldWaterWeight, nearFieldSeaLevelMeters, hasMacroGlobe,
                studioDirectLight, cpuTimingRecorder, composeStageStarted,
                resolvedOceanForView, frameIndex, drawBackgroundBodies,
                recordComposeStage, macroGlobeSurface, shape,
                acquireCelestialGrant, completeCelestialGrant};
            ComposeBodyPreviewPass(sceneContext);
            break;
        }
        case StudioViewportPresentation::FlatMap:
        {
            transitionDiagnostics_.erase(info.id);
            clipmapPlanStats_.erase(info.id);

            macroGlobePresentations_.erase(
                info.id);
            terrainPresentations_.erase(
                info.id);

            // The map itself is drawn after the output transform (see the
            // ".FlatMap" pass below) so exposure and tone mapping leave it
            // alone; the scene underneath is just cleared.
            std::optional<StudioFlatMapRenderer::SourceBinding> flatMapSource;
            std::optional<math::Double3> flatMapMarker;
            if (terrainRuntime.has_value())
            {
                flatMapSource = StudioFlatMapRenderer::SourceBinding{
                    .terrain =
                        &session.TerrainRuntime().TerrainSource(
                            *terrainRuntime),
                    .planet = terrainRuntime->planet.id,
                    .radiusMeters = terrainRuntime->planet.radiusMeters,
                    .revision =
                        terrainRuntime->terrainSourceRevision ^
                        (terrainRuntime->surfaceSourceRevision *
                         0x9E3779B97F4A7C15ULL)
                };
                if (math::LengthSquared(terrainRuntime->observer.meters) >
                    0.0)
                {
                    flatMapMarker = terrainRuntime->observer.meters;
                }
            }
            flatMapRenderer_.Advance(
                info.id,
                flatMapSource.has_value() ? &*flatMapSource : nullptr,
                info.flatMapLayer,
                flatMapMarker,
                8.0);

            graph.AddPass(
                prefix + ".FlatMapClear",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [color](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.0F,
                            .green = 0.0F,
                            .blue = 0.0F,
                            .alpha = 1.0F
                        });
                });
            break;
        }

        case StudioViewportPresentation::Blank:
        {
            transitionDiagnostics_.erase(info.id);
            clipmapPlanStats_.erase(info.id);

            macroGlobePresentations_.erase(
                info.id);
            terrainPresentations_.erase(
                info.id);

            graph.AddPass(
                prefix + ".Blank",
                {
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [color](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    commands.ClearColorTarget(
                        *color,
                        {
                            .red = 0.0F,
                            .green = 0.0F,
                            .blue = 0.0F,
                            .alpha = 1.0F
                        });
                });
            break;
        }
        }

        const bool usesPhysicalSurfaceLighting =
            compactObjectDiagnostics_.find(
                info.id) ==
                compactObjectDiagnostics_.end() &&
            (presentation ==
                 StudioViewportPresentation::ProductionTerrain ||
             presentation ==
                 StudioViewportPresentation::MacroGlobe ||
             presentation ==
                 StudioViewportPresentation::BodyPreview);

        render_graph::BufferHandle
            sharedRadianceCellsHandle{};
        render_graph::BufferHandle
            sharedRadianceLevelsHandle{};
        u32 sharedRadianceLevelCount =
            0U;

        if (usesPhysicalSurfaceLighting)
        {
            StudioLightingPassContext lightingContext{
                graph, session, view, info,
                targets, prefix, presentation, snapshot,
                atTime, frameIndex, studioDirectLight, terrainRuntime,
                lightingPlan, lightingTimestamps, cpuTimingRecorder, recordComposeStage,
                sharedRadianceCellsHandle, sharedRadianceLevelsHandle, sharedRadianceLevelCount, shape,
                bodies, frames, resolvedMagnetosphereForView, composeStageStarted,
                nearFieldWaterWeight, resolvedOceanForView, nearFieldSeaLevelMeters,
                (info.layers.terrainOccludedAtmosphere ||
                 info.layers.sunAuditMethod == 2U) &&
                    hasMacroGlobe && macroGlobeSurface != nullptr &&
                    macroGlobeSurface->terrain != nullptr &&
                    shape.has_value() && logicalTarget->target.has_value()
                    ? EnsureTerrainSunShadow(info.id, logicalTarget->target->body,
                        macroGlobeSurface->terrain, ReferenceRadiusForShape(*shape))
                    : nullptr
            };
            ComposeLightingPasses(lightingContext);
        }

        if (activeRingMesh != nullptr &&
            resolvedRingSystemForView.has_value() &&
            logicalTarget->mode !=
                studio_session::ViewportMode::Debug)
        {
            auto* ringMesh =
                activeRingMesh;
            const auto ringCamera =
                view->Camera();

            const auto normal =
                math::Normalize(
                    resolvedRingSystemForView->
                        parameters.
                        planeNormalBody);

            const math::Float3 ringNormal{
                static_cast<f32>(normal.x),
                static_cast<f32>(normal.y),
                static_cast<f32>(normal.z)
            };

            const bool receiveBodyShadow =
                resolvedRingSystemForView->
                    parameters.
                    receiveBodyShadow;

            graph.AddPass(
                prefix + ".CelestialRings",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 ringMesh,
                 ringCamera,
                 ringNormal,
                 receiveBodyShadow,
                 studioDirectLight](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    ringRenderer_.Draw(
                        commands,
                        *color,
                        width,
                        height,
                        *ringMesh,
                        ringCamera,
                        ringNormal,
                        receiveBodyShadow,
                        celestial_rings::
                            RingRenderLighting{
                                .directionBody =
                                    studioDirectLight.
                                        directionBody,
                                .irradianceScale =
                                    studioDirectLight.
                                        irradianceScale
                            });
                });
        }

        if (activeAuroraMesh != nullptr &&
            resolvedMagnetosphereForView.has_value() &&
            logicalTarget->mode !=
                studio_session::ViewportMode::Debug)
        {
            auto* auroraMesh =
                activeAuroraMesh;
            const auto auroraCamera =
                view->Camera();
            // Mesh emission is already authored in scene-linear HDR and
            // includes auroralIntensity. Keep draw scaling neutral so intensity
            // is not applied twice.
            const f32 intensityScale =
                1.0F;

            graph.AddPass(
                prefix +
                    ".CelestialAurora",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 auroraMesh,
                 auroraCamera,
                 intensityScale](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    auroraRenderer_.Draw(
                        commands,
                        *color,
                        width,
                        height,
                        *auroraMesh,
                        auroraCamera,
                        intensityScale);
                });
        }

        if (!foregroundBodies->empty())
        {
            graph.AddPass(
                prefix + ".ForegroundBodies",
                {{
                    .texture = targets.color,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                }},
                [drawForegroundBodies](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    drawForegroundBodies(commands);
                });
        }

        if (drawPathDebug &&
            presentation ==
                StudioViewportPresentation::BodyPreview &&
            frames != nullptr &&
            !products.empty())
        {
            const auto* frameGraph = frames;
            const auto pathProducts = products;
            const auto camera =
                view->Camera();

            graph.AddPass(
                prefix + ".Paths",
                {
                    {
                        .texture = targets.color,
                        .state =
                            rhi::ResourceState::
                                RenderTarget,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 color,
                 width,
                 height,
                 camera,
                 frameGraph,
                 pathProducts,
                 atTime](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    pathRenderer_.Draw(
                        commands,
                        *color,
                        width,
                        height,
                        camera,
                        *frameGraph,
                        atTime,
                        pathProducts);
                });
        }

        if (terrainRuntime.has_value() &&
            logicalTarget->mode !=
                studio_session::ViewportMode::Debug)
        {
            const auto& source =
                session.TerrainRuntime().TerrainSource(
                    *terrainRuntime);

            std::vector<
                editor_ui::PreviewLine>
                overlayLines;

            auto overlay =
                views.TerrainAuthoringOverlay(info.id);

            if (!overlay.has_value())
            {
                overlay =
                    SelectedTerrainAuthoringOverlay(
                        session,
                        *terrainRuntime);
            }

            if (overlay.has_value() &&
                overlay->body ==
                    terrainRuntime->body)
            {
                auto lines =
                    BuildTerrainAuthoringOverlayLines(
                        *overlay,
                        *terrainRuntime,
                        source,
                        view->Camera());

                overlayLines.insert(
                    overlayLines.end(),
                    lines.begin(),
                    lines.end());
            }

            const auto diagnostics =
                views.TerrainDiagnosticOverlays(
                    info.id);

            if (diagnostics.authoredConstraints)
            {
                const auto constraints =
                    TerrainConstraintDiagnosticOverlays(
                        session,
                        *terrainRuntime);

                for (const auto& constraint :
                     constraints)
                {
                    auto lines =
                        BuildTerrainAuthoringOverlayLines(
                            constraint,
                            *terrainRuntime,
                            source,
                            view->Camera());

                    overlayLines.insert(
                        overlayLines.end(),
                        lines.begin(),
                        lines.end());
                }
            }

            if (diagnostics.Any())
            {
                const auto statuses =
                    session.
                        TerrainPhysicalPages().
                        Catalog(
                            terrainRuntime->
                                planet.id);

                std::vector<
                    StudioTerrainDiagnosticPage>
                    pages;

                pages.reserve(
                    statuses.size());

                for (const auto& status :
                     statuses)
                {
                    pages.push_back({
                        .status = status,
                        .snapshot =
                            session.
                                TerrainPhysicalPages().
                                Find(
                                    status.address)
                    });
                }

                StudioClipmapActiveRange activeRange{};
                if (const auto plan = clipmapPlanStats_.find(info.id);
                    plan != clipmapPlanStats_.end() && plan->second.valid &&
                    plan->second.dynamic)
                {
                    activeRange = {
                        .valid = true,
                        .firstLevel = plan->second.firstLevel,
                        .lastLevel = plan->second.lastLevel,
                        .suppressRings = plan->second.banded};
                }

                auto lines =
                    BuildTerrainDiagnosticOverlayLines(
                        diagnostics,
                        *terrainRuntime,
                        source,
                        pages,
                        view->Camera(),
                        activeRange);

                overlayLines.insert(
                    overlayLines.end(),
                    lines.begin(),
                    lines.end());
            }

            if (!overlayLines.empty())
            {
                const auto camera =
                    view->Camera();

                auto* overlayColor =
                    color;

                graph.AddPass(
                    prefix + ".TerrainOverlays",
                    {
                        {
                            .texture = targets.color,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        }
                    },
                    [this,
                     overlayColor,
                     width,
                     height,
                     camera,
                     overlayLines = std::move(overlayLines)](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        pathRenderer_.
                            DrawCameraRelativeLines(
                                commands,
                                *overlayColor,
                                width,
                                height,
                                camera,
                                overlayLines);
                    });
            }
        }

        if (resolvedMagnetosphereForView.has_value() &&
            shape.has_value() &&
            logicalTarget->mode !=
                studio_session::ViewportMode::Debug)
        {
            auto magnetosphereLines =
                SelectedMagnetosphereDiagnosticLines(
                    session,
                    *resolvedMagnetosphereForView,
                    ReferenceRadiusForShape(*shape),
                    view->Camera());

            if (!magnetosphereLines.empty())
            {
                const auto camera = view->Camera();

                graph.AddPass(
                    prefix + ".MagnetosphereDiagnostics",
                    {
                        {
                            .texture = targets.color,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        }
                    },
                    [this,
                     color,
                     width,
                     height,
                     camera,
                     magnetosphereLines =
                         std::move(magnetosphereLines)](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        pathRenderer_.DrawCameraRelativeLines(
                            commands,
                            *color,
                            width,
                            height,
                            camera,
                            magnetosphereLines);
                    });
            }
        }

        {
            auto lightGizmoLines =
                SelectedLocalLightGizmoLines(
                    session,
                    view->Camera());

            if (!lightGizmoLines.empty())
            {
                const auto camera =
                    view->Camera();

                graph.AddPass(
                    prefix + ".LocalLightGizmo",
                    {
                        {
                            .texture = targets.color,
                            .state =
                                rhi::ResourceState::
                                    RenderTarget,
                            .access =
                                render_graph::Access::
                                    Write
                        }
                    },
                    [this,
                     color,
                     width,
                     height,
                     camera,
                     lightGizmoLines =
                        std::move(
                            lightGizmoLines)](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        pathRenderer_.
                            DrawCameraRelativeLines(
                                commands,
                                *color,
                                width,
                                height,
                                camera,
                                lightGizmoLines);
                    });
            }
        }

        ComposeVolumePasses(graph, views, session, view, info, targets, prefix,
            selectedVolume, sharedVolumeStorage, sharedVolumeFields, frameIndex,
            sharedRadianceCellsHandle, sharedRadianceLevelsHandle,
            sharedRadianceLevelCount, snapshot, atTime, studioDirectLight,
            recordComposeStage);

        // Mesh distance-field debug view: sphere traces the merged field and
        // shows it in place of (or beside) the rasterised scene.
        if (info.layers.sdfDebugView != 0U &&
            meshSdfScene_.Volume().ready &&
            view->SurfaceDebugMode() == lighting::SurfaceDebugMode::Lit &&
            width > 0U && height > 0U)
        {
            auto& scratch = sdfDebugScratch_[info.id];
            if (scratch == nullptr || scratch->Width() != width ||
                scratch->Height() != height)
            {
                scratch = device_->CreateTexture({
                    .width = width,
                    .height = height,
                    .format = rhi::TextureFormat::RGBA16_Float,
                    .initialState = rhi::ResourceState::ShaderResource,
                    .allowUnorderedAccess = true});
            }
            const auto scratchHandle = graph.ImportTexture(
                prefix + ".SdfDebugScratch",
                *scratch,
                rhi::ResourceState::ShaderResource);

            graph.AddPass(
                prefix + ".SdfDebug",
                {
                    {.texture = targets.color,
                     .state = rhi::ResourceState::ShaderResource,
                     .access = render_graph::Access::Read},
                    {.texture = scratchHandle,
                     .state = rhi::ResourceState::UnorderedAccess,
                     .access = render_graph::Access::Write}
                },
                [this,
                 color,
                 scratch = scratch.get(),
                 width,
                 height,
                 lightingView = view->Lighting(),
                 sun = studioDirectLight.directionBody,
                 mode = static_cast<u32>(info.layers.sdfDebugView)](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    meshSdfDebugRenderer_.Draw(
                        commands,
                        meshSdfScene_.Volume(),
                        *color,
                        *scratch,
                        width,
                        height,
                        lightingView,
                        sun,
                        mode);
                });
            graph.AddPass(
                prefix + ".SdfDebugCopyBack",
                {
                    {.texture = scratchHandle,
                     .state = rhi::ResourceState::ShaderResource,
                     .access = render_graph::Access::Read},
                    {.texture = targets.color,
                     .state = rhi::ResourceState::RenderTarget,
                     .access = render_graph::Access::Write}
                },
                [this,
                 color,
                 scratch = scratch.get(),
                 width,
                 height](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    debugComposite_.Draw(
                        commands, *scratch, *color, width, height);
                });
        }

        auto* displayColor = &view->DisplayColor();
        ComposePostProcess(graph, view, info, targets, prefix,
            frameIndex, antiAliasingMode);

        if (presentation == StudioViewportPresentation::FlatMap)
        {
            graph.AddPass(
                prefix + ".FlatMap",
                {
                    {
                        .texture = targets.display,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 displayColor,
                 width,
                 height,
                 viewId = info.id,
                 frameSlot = frameIndex % framesInFlight_](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    flatMapRenderer_.Draw(
                        commands,
                        *displayColor,
                        width,
                        height,
                        viewId,
                        frameSlot);
                });
        }

        if (logicalTarget->mode == studio_session::ViewportMode::BodyMap &&
            terrainRuntime.has_value())
        {
            // Graticule and observer marker over the globe view, drawn after
            // the output transform like the flat map.
            graph.AddPass(
                prefix + ".GlobeOverlay",
                {
                    {
                        .texture = targets.display,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 displayColor,
                 width,
                 height,
                 camera = view->Camera(),
                 radius = terrainRuntime->planet.radiusMeters,
                 marker = math::LengthSquared(
                              terrainRuntime->observer.meters) > 0.0
                     ? std::optional<math::Double3>(
                           terrainRuntime->observer.meters)
                     : std::nullopt](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    flatMapRenderer_.DrawGlobeOverlay(
                        commands,
                        *displayColor,
                        width,
                        height,
                        camera,
                        radius,
                        marker);
                });
        }

        rendered.push_back({
            .id = info.id,
            .targets = targets,
            .targeted = shape.has_value()
        });
        recordComposeStage("post");
    }

    for (auto iterator =
             terrainPresentations_.begin();
         iterator !=
             terrainPresentations_.end();)
    {
        const bool stillExists =
            std::find_if(
                catalog.begin(),
                catalog.end(),
                [&iterator](
                    const StudioRenderViewInfo& item)
                {
                    return item.id ==
                        iterator->first;
                }) !=
            catalog.end();

        if (!stillExists)
        {
            iterator =
                terrainPresentations_.
                    erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }

    for (std::size_t index = 0U;
         index < celestialFrameGrants.size();
         ++index)
    {
        if (celestialGrantConsumed[index])
        {
            continue;
        }

        const auto& grant =
            celestialFrameGrants[index];

        celestialScheduler_.Abandon(
            grant.key,
            grant.authorityRevision);
    }

    return rendered;
}
} // namespace orbit::studio_ui
