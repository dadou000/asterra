#pragma once

#include <orbit/time/SimulationTime.hpp>

namespace orbit::studio_session
{
class SimulationClock
{
public:
    [[nodiscard]] time::SimulationTime Time() const noexcept;
    void SetTime(time::SimulationTime value) noexcept;

    [[nodiscard]] bool Playing() const noexcept;
    void SetPlaying(bool playing) noexcept;

    [[nodiscard]] f64 Rate() const noexcept;
    void SetRate(f64 simulationSecondsPerRealSecond);

    void Advance(f64 realSeconds);
    void StepSeconds(f64 simulationSeconds);

private:
    time::SimulationTime time_{};
    bool playing_{false};
    f64 rate_{1.0};
    f64 fractionalMicroseconds_{0.0};
};
} // namespace orbit::studio_session
