#include <orbit/frames/FrameGraph.hpp>

#include <stdexcept>
#include <utility>

namespace orbit::frames
{
FrameId FrameGraph::CreateRoot()
{
    FrameId id = FrameId::Random();

    while (Contains(id))
    {
        id = FrameId::Random();
    }

    return CreateRoot(id);
}

FrameId FrameGraph::CreateRoot(
    const FrameId id)
{
    AddFrame({
        .id = id,
        .parent = std::nullopt,
        .parentFromFrame = {}
    });

    return id;
}

FrameId FrameGraph::CreateFrame(
    const FrameId parent,
    FrameTransformProvider parentFromFrame)
{
    FrameId id = FrameId::Random();

    while (Contains(id))
    {
        id = FrameId::Random();
    }

    return CreateFrame(
        id,
        parent,
        std::move(parentFromFrame));
}

FrameId FrameGraph::CreateFrame(
    const FrameId id,
    const FrameId parent,
    FrameTransformProvider parentFromFrame)
{
    AddFrame({
        .id = id,
        .parent = parent,
        .parentFromFrame =
            std::move(parentFromFrame)
    });

    return id;
}

void FrameGraph::AddFrame(
    FrameDefinition definition)
{
    if (!definition.id)
    {
        throw std::invalid_argument(
            "FrameGraph frame ID must be valid.");
    }

    if (Contains(definition.id))
    {
        throw std::invalid_argument(
            "FrameGraph frame ID already exists.");
    }

    if (definition.parent.has_value())
    {
        if (!Contains(*definition.parent))
        {
            throw std::invalid_argument(
                "FrameGraph parent frame does not exist.");
        }

        if (!definition.parentFromFrame)
        {
            throw std::invalid_argument(
                "Non-root frame requires a transform provider.");
        }
    }
    else if (definition.parentFromFrame)
    {
        throw std::invalid_argument(
            "Root frame must not have a parent transform provider.");
    }

    frames_.emplace(
        definition.id,
        FrameRecord{
            .parent = definition.parent,
            .parentFromFrame =
                std::move(
                    definition.parentFromFrame),
            .depth = definition.parent.has_value()
                ? frames_.at(*definition.parent).depth + 1
                : 0,
            .root = definition.parent.has_value()
                ? frames_.at(*definition.parent).root
                : definition.id
        });
}

bool FrameGraph::Contains(
    const FrameId frame) const noexcept
{
    return frames_.contains(frame);
}

std::optional<FrameId> FrameGraph::Parent(
    const FrameId frame) const noexcept
{
    const auto found = frames_.find(frame);

    if (found == frames_.end())
    {
        return std::nullopt;
    }

    return found->second.parent;
}

std::optional<math::RigidTransformD>
FrameGraph::ResolveTransform(
    const FrameId source,
    const FrameId target,
    const time::SimulationTime atTime) const
{
    if (source == target)
    {
        if (!Contains(source))
        {
            return std::nullopt;
        }

        return math::IdentityRigidTransformD();
    }

    auto sourceRecord = frames_.find(source);
    auto targetRecord = frames_.find(target);
    if (sourceRecord == frames_.end() ||
        targetRecord == frames_.end() ||
        sourceRecord->second.root != targetRecord->second.root)
    {
        return std::nullopt;
    }

    math::RigidTransformD
        commonFromSource =
            math::IdentityRigidTransformD();

    math::RigidTransformD
        commonFromTarget =
            math::IdentityRigidTransformD();

    // Depth is stable because frames are immutable once inserted. Walking
    // toward the common ancestor needs no per-query vectors or hash table.
    while (sourceRecord != targetRecord)
    {
        if (sourceRecord->second.depth >= targetRecord->second.depth)
        {
            if (!sourceRecord->second.parent.has_value())
            {
                return std::nullopt;
            }
            commonFromSource = math::Compose(
                sourceRecord->second.parentFromFrame(atTime),
                commonFromSource);
            sourceRecord = frames_.find(*sourceRecord->second.parent);
        }
        else
        {
            if (!targetRecord->second.parent.has_value())
            {
                return std::nullopt;
            }
            commonFromTarget = math::Compose(
                targetRecord->second.parentFromFrame(atTime),
                commonFromTarget);
            targetRecord = frames_.find(*targetRecord->second.parent);
        }
    }

    return math::Compose(
        math::Inverse(
            commonFromTarget),
        commonFromSource);
}

std::optional<FramePoint>
FrameGraph::TransformPoint(
    const FramePoint& point,
    const FrameId target,
    const time::SimulationTime atTime) const
{
    const auto targetFromSource =
        ResolveTransform(
            point.frame,
            target,
            atTime);

    if (!targetFromSource.has_value())
    {
        return std::nullopt;
    }

    return FramePoint{
        .frame = target,
        .localMeters =
            math::TransformPoint(
                *targetFromSource,
                point.localMeters)
    };
}

std::optional<math::Float3>
FrameGraph::ToCameraRelative(
    const FramePoint& point,
    const FramePoint& cameraOrigin,
    const time::SimulationTime atTime) const
{
    const auto pointInCameraFrame =
        TransformPoint(
            point,
            cameraOrigin.frame,
            atTime);

    if (!pointInCameraFrame.has_value())
    {
        return std::nullopt;
    }

    const math::Double3 delta =
        pointInCameraFrame->
            localMeters -
        cameraOrigin.localMeters;

    return math::Float3{
        static_cast<f32>(delta.x),
        static_cast<f32>(delta.y),
        static_cast<f32>(delta.z)
    };
}
} // namespace orbit::frames
