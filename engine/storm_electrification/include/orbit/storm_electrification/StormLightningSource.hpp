#pragma once

#include <orbit/storm_electrification/Electrification.hpp>
#include <orbit/storm_electrification/LeaderSimulation.hpp>

#include <cstdint>
#include <utility>
#include <vector>

namespace orbit::storm_electrification
{
// Runs a storm's electrification forward at a chosen speed-up and hands back the flashes it produces, so a host can
// forward them to the lights without knowing any physics. Fields are a frozen snapshot of the storm (the caller
// refreshes by constructing a new source); charge state is owned here.
//
// Flash graphs are in grid coordinates: x and y in [0, n * spacing), z up from the ground. Each returned flash carries the
// simulated time it started at, in seconds since the source was created.
struct StormLightningParams
{
    double timeScale{20.0};          // simulated storm seconds per wall second
    double stepSeconds{5.0};         // electrification step; flashes are tried after each
    std::uint32_t maxFlashesPerStep{5};
    ElectrificationParams electrification{};
    LeaderParams leader{};
};

// A flash's graph carries pulse and formation times on the source's clock (seconds since it was created), the same clock as
// `flash.startTime`. The lights and the flash registry want times relative to the flash itself: this moves the graph to start at 0 and
// multiplies every time by `stretch` (1 = real time). Without it a flash that starts 40 s into a run is dark for its first 40 s.
void RebaseFlashTimes(FlashResult& flash, double stretch = 1.0);

class StormLightningSource
{
public:
    StormLightningSource(StormFields fields, std::uint32_t nx, std::uint32_t ny, std::uint32_t nz, double spacingMeters,
                         StormLightningParams params = {});

    // Advances by `wallSeconds` of wall time (= wallSeconds * timeScale simulated). Whole steps only; the remainder
    // carries over, so many tiny calls and one large call give the same result.
    // `maxSteps` > 0 bounds the work of one call: when more steps are due, the surplus is dropped (the storm falls behind
    // its requested pace instead of stalling the caller, so the next call never starts further behind).
    std::vector<FlashResult> Advance(double wallSeconds, std::uint32_t maxSteps = 0);

    // Replaces the fields of a live storm (same grid) while the charge state carries on: a simulated storm changes as it runs.
    void UpdateFields(StormFields fields) { fields_ = std::move(fields); }

    [[nodiscard]] double SimulatedSeconds() const { return simulated_; }
    [[nodiscard]] double MaxBreakdownRatio() const;
    [[nodiscard]] std::uint64_t FlashCount() const { return flashes_; }
    [[nodiscard]] double DomainSizeMeters() const { return grid_.Spacing() * grid_.Nx(); }

private:
    StormFields fields_;
    ChargeGrid grid_;
    StormLightningParams params_;
    double simulated_{0.0};
    double carry_{0.0};
    std::uint64_t flashes_{0};
};
} // namespace orbit::storm_electrification
