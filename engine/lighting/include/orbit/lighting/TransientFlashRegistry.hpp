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

struct FlashStatus
{
    u64 id{0U};
    bool placed{false};
    f64 ageSeconds{0.0};
    f64 remainingSeconds{0.0};
    math::Double3 baseMeters{};
};

class TransientFlashRegistry
{
public:
    // Queues a flash that starts at `nowSeconds`. Returns its id (never 0).
    u64 Fire(const FlashRequest& request, f64 nowSeconds);
    // Adds an externally simulated flash already placed in the lighting frame. `graph` time zero is `nowSeconds`.
    u64 Add(optical_transport::DischargeGraph graph, const math::Double3& originMeters, f64 nowSeconds, f64 lifetimeSeconds);

    // Resolves pending placements, drops expired flashes, and returns the lights for [nowSeconds, +exposure).
    [[nodiscard]] std::vector<LocalLight> LightsForWindow(
        f64 nowSeconds, const CameraPlacement& camera, const TransientLightParams& params);

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
    };
    std::vector<Flash> flashes_;
    u64 nextId_{1U};
};
} // namespace orbit::lighting
