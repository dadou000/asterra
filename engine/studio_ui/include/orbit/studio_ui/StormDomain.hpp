#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rpc/JsonRpc.hpp>

#include <mutex>
#include <optional>
#include <string>

namespace orbit::studio_ui
{
// A simulated storm domain placed on the body under the primary view (phase 5 of the storm domain plan, docs:
// /world/weather-lab/global-system-plan): the GPU storm core runs in step with the simulation clock and the cloud renderer draws its
// condensate instead of the procedural clouds inside the domain.
struct StormPlaceRequest
{
    f64 latitudeDegrees{0.0};
    f64 longitudeDegrees{0.0};
    u32 cells{128};            // nx = ny, a power of two from 16 to 256
    f32 spacingMeters{2000.0F};
    f32 bubbleKelvin{2.0F};    // warm bubble that starts the storm
    f32 spinMinutes{40.0F};    // simulated before the domain first draws, so a storm exists when it appears
    bool ice{true};
    u32 spongeCells{12};
    bool nested{true};         // a 4:1 child grid (500 m for a 2 km parent) over the middle quarter of the domain
};

// Scheduled storms (phase 6): storms come from celestial_clouds::StormSchedule, a pure function of the absolute simulation time, so the
// same time shows the same storm in every launch and a time jump needs no stepping through the skipped time. The nearest live storm within
// `searchKilometres` of the camera is simulated; a manual placement (storm.place) overrides it.
struct StormAutoRequest
{
    bool enabled{false};
    u64 seed{1};
    u32 cells{128};
    f32 spacingMeters{2000.0F};
    f32 searchKilometres{500.0F};
    bool ice{true};
    bool nested{true};
};

struct StormStatus
{
    enum class State : u8
    {
        None,
        Pending,   // requested; the render thread builds it on the next cloud frame
        Active,
        Failed
    };
    State state{State::None};
    std::string mode{"off"};   // off, manual or auto
    std::string message;
    f64 latitudeDegrees{0.0};    // the domain's current centre (it drifts with the storm)
    f64 longitudeDegrees{0.0};
    f64 stormSeconds{0.0};       // simulated time in the storm, including the spin-up
    u64 steps{0};
    f64 stepMilliseconds{0.0};   // GPU wall time of the last batch, per step
    u32 cells{0};
    f32 spacingMeters{0.0F};
    f64 residentMiB{0.0};
    u64 stormId{0};              // identity of the storm being drawn (scheduled storms: stable across launches)
    f64 ageSeconds{0.0};         // simulation time since the storm began
    f64 lifetimeSeconds{0.0};
    f64 intensity{0.0};          // 1 = replaces the procedural clouds, falls to 0 as the storm dies
    bool catchingUp{false};      // the solver is stepping to the current time and nothing is drawn yet
    u32 checkpoints{0};          // saved states of this storm; a time jump restarts from the nearest
    f64 checkpointMiB{0.0};      // memory of all cached checkpoints
    u32 lightningFlashCells{0U};           // channel cells of the last flash
    f64 lightningFlashRadiusMeters{0.0};   // the largest distance from its centre to any point of the last flash's channel
    f64 lightningFlashRadiusMaxMeters{0.0}; // the largest such radius since the lightning started
    f64 lightningJobMilliseconds{0.0};  // wall time the electrification worker took for its last job (storm seconds in, flashes and charge out)
    u32 lightningArcSegments{0U};  // channel segments the arc overlay drew in the last frame that had any
    u64 lightningGroundFlashes{0U};   // of those, the ones that reached the ground (cloud to ground, with a return stroke)
    u64 lightningFlashes{0U};    // flashes the storm's own charge has produced since it started (or since the last time jump)
    u32 scheduledNearby{0};      // live scheduled storms within the search radius
    bool nested{false};          // the storm being drawn has a child grid
    f32 childSpacingMeters{0.0F};
    std::string shaderReload;    // the last storm shader reload: "" never, "ok <file>" or "failed: <compile error>"
    u32 shaderReloads{0};        // successful reloads so far
};

// The lightning the simulated storm produces from its own charge (storm.lightning). The last three are for testing: a real storm flashes
// for milliseconds a few times a minute, which is hard to look at; `slowMotion` stretches every flash (keeping its power), `chargeBoost`
// multiplies the charging rate and `breakdownScale` lowers the field at which air breaks down, so the storm is electrically unstable.
struct StormLightningSettings
{
    bool enabled{true};
    f64 slowMotion{1.0};      // 1 to 2000
    f64 chargeBoost{1.0};     // 1 to 10000
    f64 breakdownScale{1.0};  // 0.02 to 1
    f64 brightness{1.0};      // 1 to 1e6: multiplies the energy of every flash (the lights on the clouds and the arc)
    bool arcs{true};          // draw the channel of a live flash as a glowing line
    bool pauseOnStrike{false}; // pause the simulation just before a flash that is in view starts, to play it in slow motion
    bool frameStrike{false};   // aim the camera at a flash about to start and back off until its whole channel is in view
    bool showGrid{false};      // draw the electrification window (its box, a grid and the charge in it)
    f64 flashSize{4.0};        // 1 to 50: multiplies the charge one flash may neutralise, so a larger value gives larger flashes
};

// The charge in the electrified window summed over blocks of cells, for the grid display: coulombs per block, x fastest then y then z.
struct StormChargeBlocks
{
    u32 nx{0};
    u32 ny{0};
    u32 nz{0};
    f32 blockMeters{0.0F};
    std::vector<f32> coulombs;
};

[[nodiscard]] const char* StormStateName(StormStatus::State state) noexcept;

// Thread-safe hand-off between the RPC / panel threads and the render thread, which owns the GPU solver: requests are queued here and
// built and stepped where the Vulkan queue is used, never on the caller's thread.
class StormDomainMailbox
{
public:
    // Validates and queues a placement; returns the problem, or an empty string.
    [[nodiscard]] std::string Place(const StormPlaceRequest& request);
    void Clear();
    [[nodiscard]] StormStatus Status() const;
    // Automatic (scheduled) storms; validates and queues.
    [[nodiscard]] std::string SetAuto(const StormAutoRequest& request);
    [[nodiscard]] StormAutoRequest AutoSetting() const;
    // Queues a reload of the storm kernels from `path` (empty: the embedded copy). The hot-iteration handler and storm.reload_shaders both
    // call this; the render thread does the reload.
    void RequestShaderReload(const std::string& path);
    // Validates and stores the lightning settings; returns the problem, or an empty string. Read by the render thread every frame.
    [[nodiscard]] std::string SetLightning(const StormLightningSettings& settings);
    [[nodiscard]] StormLightningSettings Lightning() const;

    // Render thread.
    [[nodiscard]] std::optional<StormPlaceRequest> TakePlace();
    [[nodiscard]] bool TakeClear();
    [[nodiscard]] std::optional<StormAutoRequest> TakeAuto();
    [[nodiscard]] std::optional<std::string> TakeShaderReload();
    void PublishShaderReload(const std::string& message, bool succeeded);
    void Publish(const StormStatus& status);

private:
    mutable std::mutex mutex_;
    std::optional<StormPlaceRequest> place_;
    bool clear_{false};
    std::optional<StormAutoRequest> auto_;
    std::optional<std::string> reload_;
    std::string reloadMessage_;
    u32 reloadCount_{0};
    StormAutoRequest autoSetting_;
    StormLightningSettings lightning_;
    StormStatus status_;
};

// storm.place / storm.status / storm.clear: the RPC face of the Weather Lab panel's Storm domain section (docs/ORBIT_MCP.md).
// `mailbox` must outlive the dispatcher.
void RegisterStormDomainRpc(rpc::Dispatcher& dispatcher, StormDomainMailbox& mailbox);
} // namespace orbit::studio_ui
