#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orbit::celestial_gravity
{
struct GravitySourceIdTag;
using GravitySourceId = core::StrongId<GravitySourceIdTag>;

class GravityModel
{
public:
    virtual ~GravityModel() = default;

    [[nodiscard]] virtual math::Double3 AccelerationLocal(
        math::Double3 sourceToPointMeters) const = 0;

    [[nodiscard]] virtual std::string_view ModelName() const noexcept = 0;
};

class PointMassGravityModel final : public GravityModel
{
public:
    explicit PointMassGravityModel(
        f64 gravitationalParameterM3PerS2,
        f64 softeningMeters = 0.0);

    [[nodiscard]] math::Double3 AccelerationLocal(
        math::Double3 sourceToPointMeters) const override;

    [[nodiscard]] std::string_view ModelName() const noexcept override;

    [[nodiscard]] f64 GravitationalParameter() const noexcept;

private:
    f64 mu_{0.0};
    f64 softeningMeters_{0.0};
};

struct GravitySource
{
    GravitySourceId id{};
    frames::FrameId frame{};
    std::shared_ptr<const GravityModel> model;
};

class GravityService
{
public:
    explicit GravityService(
        const frames::FrameGraph& frames);

    void RegisterSource(GravitySource source);

    [[nodiscard]] bool Contains(
        GravitySourceId source) const noexcept;

    [[nodiscard]] std::vector<GravitySourceId>
    Sources() const;

    [[nodiscard]] std::optional<math::Double3>
    AccelerationFrom(
        GravitySourceId source,
        const frames::FramePoint& point,
        time::SimulationTime atTime) const;

    [[nodiscard]] std::optional<math::Double3>
    TotalAcceleration(
        const frames::FramePoint& point,
        time::SimulationTime atTime) const;

private:
    const frames::FrameGraph& frames_;
    std::unordered_map<GravitySourceId, GravitySource> sources_;
};

inline constexpr f64 kGravitationalConstant =
    6.67430e-11;

[[nodiscard]] constexpr f64
GravitationalParameterFromMass(
    const f64 massKilograms) noexcept
{
    return kGravitationalConstant * massKilograms;
}
} // namespace orbit::celestial_gravity
