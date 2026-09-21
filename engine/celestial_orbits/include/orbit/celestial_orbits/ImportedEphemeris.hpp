#pragma once

#include <orbit/celestial_orbits/OrbitState.hpp>

#include <optional>
#include <string>
#include <vector>

namespace orbit::celestial_orbits
{
struct EphemerisSample
{
    time::SimulationTime time{};
    math::Double3 positionMeters{};
    math::Double3 velocityMetersPerSecond{};
};

struct EphemerisRange
{
    time::SimulationTime first{};
    time::SimulationTime last{};
};

class ImportedEphemerisProvider final : public OrbitStateProvider
{
public:
    explicit ImportedEphemerisProvider(
        std::vector<EphemerisSample> samples,
        std::string sourceName = {});

    [[nodiscard]] OrbitState EvaluateState(
        time::SimulationTime atTime) const override;

    [[nodiscard]] std::string_view ModelName() const noexcept override;

    [[nodiscard]] const EphemerisRange& Range() const noexcept;
    [[nodiscard]] bool ContainsTime(
        time::SimulationTime atTime) const noexcept;
    [[nodiscard]] std::string_view SourceName() const noexcept;

private:
    std::vector<EphemerisSample> samples_;
    EphemerisRange range_{};
    std::string sourceName_;
};
} // namespace orbit::celestial_orbits
