#include <orbit/editor_model/SystemViewModel.hpp>

#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numbers>
#include <span>
#include <stdexcept>

namespace orbit::editor_model
{
SystemViewModel::SystemViewModel(
    scene::ObjectStore& objects,
    world_model::UniverseComposition& universe,
    selection::SelectionService& selection,
    commands::CommandService& commands)
    : objects_(objects),
      universe_(universe),
      selection_(selection),
      commands_(commands)
{
}

std::vector<scene::ObjectRecord>
SystemViewModel::Systems() const
{
    std::vector<scene::ObjectRecord> result;
    std::vector<scene::ObjectRecord> pending =
        objects_.Roots();

    while (!pending.empty())
    {
        const auto object =
            pending.back();
        pending.pop_back();

        if (object.type ==
            world_model::kCelestialSystemType)
        {
            result.push_back(object);
        }

        const auto children =
            objects_.Children(object.id);

        pending.insert(
            pending.end(),
            children.begin(),
            children.end());
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const auto& lhs, const auto& rhs)
        {
            if (lhs.sortOrder != rhs.sortOrder)
            {
                return lhs.sortOrder < rhs.sortOrder;
            }
            return lhs.id.ToString() < rhs.id.ToString();
        });

    return result;
}

bool SystemViewModel::DescendsFrom(
    const scene::ObjectId object,
    const scene::ObjectId ancestor) const
{
    auto current =
        objects_.Find(object);

    for (u32 depth = 0; depth < 256U;
         ++depth)
    {
        if (!current.has_value())
        {
            return false;
        }

        if (current->id == ancestor)
        {
            return true;
        }

        if (!current->parent.has_value())
        {
            return false;
        }

        current =
            objects_.Find(
                *current->parent);
    }

    throw std::runtime_error(
        "Celestial hierarchy exceeded the supported traversal safety depth.");
}

std::optional<scene::ObjectRecord>
SystemViewModel::SystemForSelection() const
{
    if (selection_.Ordered().empty())
    {
        return std::nullopt;
    }

    auto current =
        objects_.Find(
            selection_.Ordered().front());

    for (u32 depth = 0; depth < 256U;
         ++depth)
    {
        if (!current.has_value())
        {
            return std::nullopt;
        }

        if (current->type ==
            world_model::kCelestialSystemType)
        {
            return current;
        }

        if (!current->parent.has_value())
        {
            return std::nullopt;
        }

        current =
            objects_.Find(
                *current->parent);
    }

    throw std::runtime_error(
        "Celestial hierarchy exceeded the supported traversal safety depth.");
}

std::vector<SystemViewItem>
SystemViewModel::Items(
    const scene::ObjectId system,
    const time::SimulationTime atTime) const
{
    const auto systemRecord =
        objects_.Find(system);

    if (!systemRecord.has_value() ||
        systemRecord->type !=
            world_model::kCelestialSystemType)
    {
        throw std::invalid_argument(
            "System view requires a Celestial System semantic object.");
    }

    const auto systemFrame =
        universe_.FrameForObject(system);

    if (!systemFrame.has_value())
    {
        throw std::runtime_error(
            "Selected system has no composed FrameGraph frame.");
    }

    std::vector<SystemViewItem> result;
    std::vector<scene::ObjectRecord> pending =
        objects_.Children(system);

    while (!pending.empty())
    {
        const auto object =
            pending.back();
        pending.pop_back();

        const auto children =
            objects_.Children(object.id);

        pending.insert(
            pending.end(),
            children.begin(),
            children.end());

        SystemViewObjectKind kind{};

        if (object.type ==
            world_model::kCelestialBodyType)
        {
            kind =
                SystemViewObjectKind::Body;
        }
        else if (object.type ==
                 world_model::
                     kCelestialReferenceNodeType)
        {
            kind =
                SystemViewObjectKind::
                    ReferenceNode;
        }
        else
        {
            continue;
        }

        const auto objectFrame =
            universe_.FrameForObject(
                object.id);

        if (!objectFrame.has_value())
        {
            continue;
        }

        const auto systemFromObject =
            universe_.Frames().
                ResolveTransform(
                    *objectFrame,
                    *systemFrame,
                    atTime);

        if (!systemFromObject.has_value())
        {
            continue;
        }

        const auto position =
            systemFromObject->
                translation;

        result.push_back({
            .object = object.id,
            .name = object.name,
            .kind = kind,
            .positionMeters = position,
            .distanceFromSystemOriginMeters =
                math::Length(position)
        });
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const auto& lhs, const auto& rhs)
        {
            if (lhs.distanceFromSystemOriginMeters !=
                rhs.distanceFromSystemOriginMeters)
            {
                return
                    lhs.distanceFromSystemOriginMeters <
                    rhs.distanceFromSystemOriginMeters;
            }

            return lhs.object.ToString() <
                rhs.object.ToString();
        });

    return result;
}

std::vector<SystemViewTrajectorySample>
SystemViewModel::SampleTrajectory(
    const scene::ObjectId object,
    const scene::ObjectId system,
    const time::SimulationTime start,
    const f64 durationSeconds,
    const u32 segments) const
{
    if (!std::isfinite(durationSeconds) ||
        segments == 0U)
    {
        throw std::invalid_argument(
            "System-view trajectory sampling requires finite duration and at least one segment.");
    }

    if (!DescendsFrom(object, system))
    {
        throw std::invalid_argument(
            "Trajectory object is not part of the selected system.");
    }

    const auto systemFrame =
        universe_.FrameForObject(system);
    const auto objectFrame =
        universe_.FrameForObject(object);

    if (!systemFrame.has_value() ||
        !objectFrame.has_value())
    {
        throw std::runtime_error(
            "Trajectory sampling requires composed FrameGraph frames.");
    }

    std::vector<SystemViewTrajectorySample>
        result;
    result.reserve(
        static_cast<std::size_t>(
            segments) +
        1U);

    for (u32 index = 0;
         index <= segments;
         ++index)
    {
        const f64 fraction =
            static_cast<f64>(index) /
            static_cast<f64>(segments);

        const f64 offsetSeconds =
            durationSeconds * fraction;

        const auto atTime =
            start +
            std::chrono::microseconds(
                static_cast<i64>(
                    std::llround(
                        offsetSeconds *
                        1'000'000.0)));

        const auto transform =
            universe_.Frames().
                ResolveTransform(
                    *objectFrame,
                    *systemFrame,
                    atTime);

        if (!transform.has_value())
        {
            throw std::runtime_error(
                "Trajectory sampling failed to resolve a FrameGraph transform.");
        }

        result.push_back({
            .time = atTime,
            .positionMeters =
                transform->translation
        });
    }

    return result;
}

std::optional<OrbitManipulationTarget>
SystemViewModel::AnalyticOrbitTarget(
    const scene::ObjectId object) const
{
    const auto body =
        objects_.Find(object);

    if (!body.has_value() ||
        body->type !=
            world_model::kCelestialBodyType)
    {
        return std::nullopt;
    }

    for (const auto& child :
         objects_.Children(object))
    {
        if (child.type !=
            world_model::kOrbitCapabilityType)
        {
            continue;
        }

        const auto model =
            objects_.GetProperty(
                child.id,
                world_model::kCapabilityModel);

        if (!model.has_value())
        {
            continue;
        }

        const auto* modelText =
            std::get_if<std::string>(
                &*model);

        if (modelText == nullptr ||
            *modelText != "Analytic Conic")
        {
            continue;
        }

        const auto readFloat =
            [&](const schema::PropertyId property,
                const f64 fallback)
            {
                const auto value =
                    objects_.GetProperty(
                        child.id,
                        property);

                if (!value.has_value())
                {
                    return fallback;
                }

                const auto* number =
                    std::get_if<f64>(
                        &*value);

                return number != nullptr
                    ? *number
                    : fallback;
            };

        return OrbitManipulationTarget{
            .body = object,
            .capability = child.id,
            .values = {
                .semiMajorAxisMeters =
                    readFloat(
                        world_model::
                            kOrbitSemiMajorAxisMeters,
                        1.0),
                .periapsisDistanceMeters =
                    readFloat(
                        world_model::
                            kOrbitPeriapsisDistanceMeters,
                        1.0),
                .eccentricity =
                    readFloat(
                        world_model::
                            kOrbitEccentricity,
                        0.0),
                .inclinationDegrees =
                    readFloat(
                        world_model::
                            kOrbitInclinationDegrees,
                        0.0),
                .ascendingNodeDegrees =
                    readFloat(
                        world_model::
                            kOrbitAscendingNodeDegrees,
                        0.0),
                .argumentPeriapsisDegrees =
                    readFloat(
                        world_model::
                            kOrbitArgumentPeriapsisDegrees,
                        0.0),
                .meanAnomalyEpochDegrees =
                    readFloat(
                        world_model::
                            kOrbitMeanAnomalyEpochDegrees,
                        0.0),
                .gravitationalParameterM3PerS2 =
                    readFloat(
                        world_model::
                            kOrbitGravitationalParameter,
                        1.0)
            }
        };
    }

    return std::nullopt;
}

void SystemViewModel::ApplyAnalyticOrbitEdit(
    const scene::ObjectId capability,
    const AnalyticOrbitEdit& edit)
{
    const auto record =
        objects_.Find(capability);

    if (!record.has_value() ||
        record->type !=
            world_model::kOrbitCapabilityType)
    {
        throw std::invalid_argument(
            "Orbit manipulation requires an Orbit / Ephemeris capability.");
    }

    const auto finite =
        [](const f64 value)
        {
            return std::isfinite(value);
        };

    if (!finite(edit.semiMajorAxisMeters) ||
        !finite(edit.periapsisDistanceMeters) ||
        !finite(edit.eccentricity) ||
        !finite(edit.inclinationDegrees) ||
        !finite(edit.ascendingNodeDegrees) ||
        !finite(edit.argumentPeriapsisDegrees) ||
        !finite(edit.meanAnomalyEpochDegrees) ||
        !finite(edit.gravitationalParameterM3PerS2) ||
        edit.eccentricity < 0.0 ||
        edit.gravitationalParameterM3PerS2 <= 0.0)
    {
        throw std::invalid_argument(
            "Analytic orbit edit contains invalid or non-finite values.");
    }

    const bool parabolic =
        std::abs(
            edit.eccentricity - 1.0) <=
        1.0e-10;

    if (parabolic)
    {
        if (edit.periapsisDistanceMeters <= 0.0)
        {
            throw std::invalid_argument(
                "Parabolic orbit requires positive periapsis distance.");
        }
    }
    else if (edit.semiMajorAxisMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Elliptic/hyperbolic orbit requires positive semi-major-axis magnitude.");
    }

    commands_.BeginTransaction(
        "Manipulate Analytic Orbit");

    try
    {
        commands_.SetProperty(
            capability,
            world_model::
                kOrbitSemiMajorAxisMeters,
            edit.semiMajorAxisMeters);
        commands_.SetProperty(
            capability,
            world_model::
                kOrbitPeriapsisDistanceMeters,
            edit.periapsisDistanceMeters);
        commands_.SetProperty(
            capability,
            world_model::
                kOrbitEccentricity,
            edit.eccentricity);
        commands_.SetProperty(
            capability,
            world_model::
                kOrbitInclinationDegrees,
            edit.inclinationDegrees);
        commands_.SetProperty(
            capability,
            world_model::
                kOrbitAscendingNodeDegrees,
            edit.ascendingNodeDegrees);
        commands_.SetProperty(
            capability,
            world_model::
                kOrbitArgumentPeriapsisDegrees,
            edit.argumentPeriapsisDegrees);
        commands_.SetProperty(
            capability,
            world_model::
                kOrbitMeanAnomalyEpochDegrees,
            edit.meanAnomalyEpochDegrees);
        commands_.SetProperty(
            capability,
            world_model::
                kOrbitGravitationalParameter,
            edit.gravitationalParameterM3PerS2);

        commands_.CommitTransaction();
    }
    catch (...)
    {
        if (commands_.HasActiveTransaction())
        {
            commands_.RollbackTransaction();
        }
        throw;
    }
}

void SystemViewModel::SetReferenceNodePosition(
    const scene::ObjectId object,
    const math::Double3 positionMeters)
{
    const auto record =
        objects_.Find(object);

    if (!record.has_value() ||
        record->type !=
            world_model::
                kCelestialReferenceNodeType)
    {
        throw std::invalid_argument(
            "Reference-node manipulation requires a reference/barycenter node.");
    }

    if (!std::isfinite(positionMeters.x) ||
        !std::isfinite(positionMeters.y) ||
        !std::isfinite(positionMeters.z))
    {
        throw std::invalid_argument(
            "Reference-node position must be finite.");
    }

    commands_.SetProperty(
        object,
        world_model::
            kReferenceNodePositionMeters,
        positionMeters);
}

f64 SystemViewModel::
SuggestedTrajectoryDurationSeconds(
    const scene::ObjectId object) const
{
    const auto target =
        AnalyticOrbitTarget(object);

    if (!target.has_value())
    {
        return 86'400.0;
    }

    const auto& orbit =
        target->values;

    if (orbit.eccentricity < 1.0 - 1.0e-10 &&
        orbit.semiMajorAxisMeters > 0.0 &&
        orbit.gravitationalParameterM3PerS2 > 0.0)
    {
        return
            2.0 *
            std::numbers::pi_v<f64> *
            std::sqrt(
                orbit.semiMajorAxisMeters *
                orbit.semiMajorAxisMeters *
                orbit.semiMajorAxisMeters /
                orbit.
                    gravitationalParameterM3PerS2);
    }

    return 7.0 * 86'400.0;
}

void SystemViewModel::Select(
    const scene::ObjectId object)
{
    if (!objects_.Find(object).has_value())
    {
        throw std::invalid_argument(
            "System-view selection references an unknown object.");
    }

    const std::array selected{object};
    selection_.Set(
        std::span(selected));
}
} // namespace orbit::editor_model
