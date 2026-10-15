#pragma once

#include <orbit/lighting/TransientLights.hpp>

#include <vector>

namespace orbit::lighting
{
// Owns the lightning flashes currently alive in a viewport and turns them into ordinary local lights for a frame.
// It is the single producer the lighting pass and the cloud pass both read, so a flash is lit and scattered into
// clouds from the same data. Flashes are fired by RPC/MCP (`lightning.*`); a physical storm simulation can later call
// `Add` with its own DischargeGraph without touching either consumer.
//
// Time is a caller-supplied monotonic clock in seconds. Placement may be deferred until the first frame so a flash can
// be requested "N metres in front of the camera" without the requester knowing the camera.
struct FlashRequest
{
    // Absolute placement of the channel base in the lighting frame, used when `cameraRelative` is false.
    math::Double3 baseMeters{};
    // When true the base is `distanceMeters` along the camera's forward direction (flattened onto the horizontal plane
    // given by `up`) and `lateralMeters` to its right, resolved at the first frame.
    bool cameraRelative{true};
    f64 distanceMeters{3000.0};
    f64 lateralMeters{0.0};
    f64 heightMeters{6000.0};           // channel length along `up`
    f64 energyJoules{3.0e5};            // optical energy over all strokes
    u32 strokes{3U};
    f64 strokeSpacingSeconds{0.06};
    u32 seed{1U};
};

struct CameraPlacement
{
    math::Double3 positionMeters{};
    math::Double3 forward{0.0, 0.0, 1.0};
    math::Double3 up{0.0, 1.0, 0.0};
};

// A fixed place in the world for flashes whose graph is in storm coordinates (x right, y forward, z up from the ground,
// the storm domain spanning [0, domainMeters) horizontally). Resolved once, at the first frame after it is set, then
// kept so the storm does not follow the camera.
struct StormAnchor
{
    f64 distanceMeters{12000.0};   // from the camera to the domain centre, along the flattened forward direction
    f64 lateralMeters{0.0};
    f64 domainMeters{32000.0};
};

struct FlashStatus
{
    u64 id{0U};
    bool placed{false};
    f64 ageSeconds{0.0};
    f64 remainingSeconds{0.0};
    math::Double3 baseMeters{};
};

// One segment of a live flash's channel, in the lighting frame, with the optical power it radiates now.
struct ChannelLine
{
    math::Double3 from{};
    math::Double3 to{};
    f64 powerWatts{0.0};
    f64 lengthMeters{0.0};
    f64 temperatureKelvin{0.0};   // power-weighted mean of the pulses lit now (0 = none gave one)
    f64 flashPeakWatts{0.0};   // the strongest segment of the same flash at this moment (so powerWatts / flashPeakWatts is the share of the trunk)
};

// A placed flash that has not started yet (or only just), with a sphere around its channel in the lighting frame.
struct UpcomingFlash
{
    u64 id{0U};
    f64 secondsUntilStart{0.0};   // negative once it has begun
    math::Double3 centre{};
    f64 radiusMeters{0.0};
};

class TransientFlashRegistry
{
public:
    // Queues a flash that starts at `nowSeconds`. Returns its id (never 0).
    u64 Fire(const FlashRequest& request, f64 nowSeconds);
    // Adds an externally simulated flash already placed in the lighting frame. `graph` time zero is `nowSeconds`.
    u64 Add(optical_transport::DischargeGraph graph, const math::Double3& originMeters, f64 nowSeconds, f64 lifetimeSeconds);

    // Sets (or moves) the storm anchor; flashes added with AddStormFlash are placed against it.
    void SetStormAnchor(const StormAnchor& anchor);
    [[nodiscard]] bool HasStormAnchor() const { return anchorRequested_; }
    // Adds a flash in storm coordinates. Its lifetime is the end of its last pulse plus a short tail.
    u64 AddStormFlash(optical_transport::DischargeGraph graph, f64 nowSeconds);

    // Resolves pending placements, drops expired flashes, and returns the lights for [nowSeconds, +exposure).
    [[nodiscard]] std::vector<LocalLight> LightsForWindow(
        f64 nowSeconds, const CameraPlacement& camera, const TransientLightParams& params);

    // The channel segments that are lit at `nowSeconds` (a pulse in progress, or one that ended less than a few `afterglowSeconds` ago and
    // fades exponentially), brightest first, at most `maxLines`: what an arc overlay draws. Flashes not yet placed are skipped.
    [[nodiscard]] std::vector<ChannelLine> ChannelLines(f64 nowSeconds, f64 afterglowSeconds, f64 minimumPowerWatts, std::size_t maxLines) const;

    // The same, for the window [t0, t1] of the flash clock instead of an instant: each segment's power is its emitted energy in the window divided
    // by the window length, exactly what LightsForWindow(t0, ..., exposure = t1 - t0) integrates, so the arcs and the lights of a frame always
    // show the same emission (a stroke shorter than the frame is never missed by one and counted by the other).
    [[nodiscard]] std::vector<ChannelLine> ChannelLinesInWindow(f64 t0, f64 t1, f64 minimumPowerWatts, std::size_t maxLines) const;

    // Placed flashes that start within `horizonSeconds` from `nowSeconds`, or began less than `graceSeconds` ago.
    [[nodiscard]] std::vector<UpcomingFlash> UpcomingFlashes(f64 nowSeconds, f64 horizonSeconds, f64 graceSeconds) const;

    [[nodiscard]] std::vector<FlashStatus> Status(f64 nowSeconds) const;
    void Clear();
    [[nodiscard]] bool Empty() const { return flashes_.empty(); }

    // A vertical return-stroke channel: `strokes` re-brightenings of one tortuous path, energy split evenly.
    // Built in the basis (right, up, forward) so the channel follows the local vertical.
    [[nodiscard]] static optical_transport::DischargeGraph BuildCannedFlash(
        const FlashRequest& request, const math::Double3& right, const math::Double3& up);

private:
    struct Flash
    {
        u64 id{0U};
        FlashRequest request;
        bool placed{false};
        math::Double3 origin{};
        optical_transport::DischargeGraph graph{optical_transport::FlashKind::CloudToGround};
        f64 startSeconds{0.0};
        f64 lifetimeSeconds{0.0};
        bool stormCoordinates{false};   // graph awaits the storm anchor
    };
    std::vector<Flash> flashes_;
    StormAnchor anchor_;
    bool anchorRequested_{false};
    bool anchorResolved_{false};
    math::Double3 anchorOrigin_{}, anchorRight_{1.0, 0.0, 0.0}, anchorUp_{0.0, 1.0, 0.0}, anchorForward_{0.0, 0.0, 1.0};
    u64 nextId_{1U};
};
} // namespace orbit::lighting
