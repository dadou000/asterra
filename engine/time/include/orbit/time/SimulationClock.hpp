#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <chrono>

namespace orbit::time
{
struct SimulationClockDesc
{
    SimulationTime epoch{};
    std::chrono::microseconds fixedStep{
        std::chrono::microseconds(16'667)};
    u32 maximumFixedStepsPerAdvance{8};
};

struct SimulationAdvance
{
    u32 fixedSteps{0};
    SimulationTime fixedTime{};
    SimulationTime renderTime{};
    f64 interpolationAlpha{0.0};
};

class SimulationClock
{
public:
    explicit SimulationClock(
        const SimulationClockDesc& desc = {});

    void SetPaused(bool paused) noexcept;
    [[nodiscard]] bool Paused() const noexcept;

    void SetTimeScale(f64 scale);
    [[nodiscard]] f64 TimeScale() const noexcept;

    [[nodiscard]] std::chrono::microseconds
    FixedStep() const noexcept;

    [[nodiscard]] SimulationTime
    FixedTime() const noexcept;

    [[nodiscard]] SimulationTime
    RenderTime() const noexcept;

    [[nodiscard]] f64
    InterpolationAlpha() const noexcept;

    [[nodiscard]] SimulationAdvance Advance(
        std::chrono::microseconds realElapsed);

    // Manual fixed stepping is valid in either paused or running mode.
    // It advances authoritative simulation time immediately and leaves
    // interpolation accumulation unchanged.
    [[nodiscard]] SimulationAdvance Step(
        u32 fixedStepCount = 1);

private:
    [[nodiscard]] SimulationAdvance Snapshot(
        u32 fixedSteps) const noexcept;

    SimulationTime fixedTime_{};
    std::chrono::microseconds fixedStep_{};
    i64 accumulatorMicroseconds_{0};
    u32 maximumFixedStepsPerAdvance_{8};
    f64 timeScale_{1.0};
    bool paused_{false};
};
} // namespace orbit::time
