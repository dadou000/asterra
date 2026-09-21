#include <orbit/celestial_gravity/GravityService.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::celestial_gravity
{
PointMassGravityModel::PointMassGravityModel(
    const f64 gravitationalParameterM3PerS2,
    const f64 softeningMeters)
    : mu_(gravitationalParameterM3PerS2),
      softeningMeters_(softeningMeters)
{
    if (!std::isfinite(mu_) ||
        mu_ < 0.0 ||
        !std::isfinite(softeningMeters_) ||
        softeningMeters_ < 0.0)
    {
        throw std::invalid_argument(
            "Point-mass gravity parameters are invalid.");
    }
}

math::Double3
PointMassGravityModel::AccelerationLocal(
    const math::Double3 sourceToPointMeters) const
{
    const f64 distance2 =
        math::Dot(
            sourceToPointMeters,
            sourceToPointMeters) +
        softeningMeters_ * softeningMeters_;

    if (distance2 <= 0.0 ||
        mu_ == 0.0)
    {
        return {};
    }

    const f64 inverseDistance =
        1.0 / std::sqrt(distance2);
    const f64 inverseDistance3 =
        inverseDistance *
        inverseDistance *
        inverseDistance;

    return sourceToPointMeters *
        (-mu_ * inverseDistance3);
}

std::string_view
PointMassGravityModel::ModelName() const noexcept
{
    return "Point Mass";
}

f64 PointMassGravityModel::
GravitationalParameter() const noexcept
{
    return mu_;
}

GravityService::GravityService(
    const frames::FrameGraph& frames)
    : frames_(frames)
{
}

void GravityService::RegisterSource(
    GravitySource source)
{
    if (!source.id ||
        !source.frame ||
        !source.model)
    {
        throw std::invalid_argument(
            "Gravity source is incomplete.");
    }

    if (!frames_.Contains(source.frame))
    {
        throw std::invalid_argument(
            "Gravity source frame does not exist.");
    }

    if (sources_.contains(source.id))
    {
        throw std::invalid_argument(
            "Gravity source ID already exists.");
    }

    sources_.emplace(
        source.id,
        std::move(source));
}

bool GravityService::Contains(
    const GravitySourceId source) const noexcept
{
    return sources_.contains(source);
}

std::vector<GravitySourceId>
GravityService::Sources() const
{
    std::vector<GravitySourceId> result;
    result.reserve(sources_.size());

    for (const auto& [id, source] : sources_)
    {
        static_cast<void>(source);
        result.push_back(id);
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const GravitySourceId lhs,
           const GravitySourceId rhs)
        {
            if (lhs.high != rhs.high)
            {
                return lhs.high < rhs.high;
            }
            return lhs.low < rhs.low;
        });

    return result;
}

std::optional<math::Double3>
GravityService::AccelerationFrom(
    const GravitySourceId source,
    const frames::FramePoint& point,
    const time::SimulationTime atTime) const
{
    const auto found =
        sources_.find(source);

    if (found == sources_.end() ||
        !frames_.Contains(point.frame))
    {
        return std::nullopt;
    }

    const auto pointInSourceFrame =
        frames_.TransformPoint(
            point,
            found->second.frame,
            atTime);

    const auto pointFromSource =
        frames_.ResolveTransform(
            found->second.frame,
            point.frame,
            atTime);

    if (!pointInSourceFrame.has_value() ||
        !pointFromSource.has_value())
    {
        return std::nullopt;
    }

    const math::Double3 accelerationSourceFrame =
        found->second.model->
            AccelerationLocal(
                pointInSourceFrame->
                    localMeters);

    return math::TransformVector(
        pointFromSource->rotation,
        accelerationSourceFrame);
}

std::optional<math::Double3>
GravityService::TotalAcceleration(
    const frames::FramePoint& point,
    const time::SimulationTime atTime) const
{
    if (!frames_.Contains(point.frame))
    {
        return std::nullopt;
    }

    math::Double3 result{};

    for (const auto source : Sources())
    {
        const auto acceleration =
            AccelerationFrom(
                source,
                point,
                atTime);

        if (!acceleration.has_value())
        {
            return std::nullopt;
        }

        result =
            result + *acceleration;
    }

    return result;
}
} // namespace orbit::celestial_gravity
