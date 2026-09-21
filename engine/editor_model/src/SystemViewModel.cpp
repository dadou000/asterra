#include <orbit/editor_model/SystemViewModel.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <stdexcept>

namespace orbit::editor_model
{
SystemViewModel::SystemViewModel(
    scene::ObjectStore& objects,
    world_model::UniverseComposition& universe,
    selection::SelectionService& selection)
    : objects_(objects),
      universe_(universe),
      selection_(selection)
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
