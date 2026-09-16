#pragma once

#include <orbit/core/Types.hpp>

#include <chrono>

namespace orbit::time
{
struct SimulationTime
{
    i64 microsecondsFromEpoch{0};

    [[nodiscard]] constexpr bool operator==(
        const SimulationTime&) const noexcept = default;

    [[nodiscard]] constexpr auto operator<=>(
        const SimulationTime&) const noexcept = default;
};

[[nodiscard]] constexpr SimulationTime operator+(
    const SimulationTime time,
    const std::chrono::microseconds delta) noexcept
{
    return {
        .microsecondsFromEpoch =
            time.microsecondsFromEpoch +
            delta.count()
    };
}

[[nodiscard]] constexpr SimulationTime operator-(
    const SimulationTime time,
    const std::chrono::microseconds delta) noexcept
{
    return {
        .microsecondsFromEpoch =
            time.microsecondsFromEpoch -
            delta.count()
    };
}

[[nodiscard]] constexpr std::chrono::microseconds
operator-(
    const SimulationTime lhs,
    const SimulationTime rhs) noexcept
{
    return std::chrono::microseconds(
        lhs.microsecondsFromEpoch -
        rhs.microsecondsFromEpoch);
}
} // namespace orbit::time
