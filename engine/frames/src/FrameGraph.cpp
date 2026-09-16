#include <orbit/frames/FrameGraph.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
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
                    definition.parentFromFrame)
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

std::vector<FrameId>
FrameGraph::AncestorsInclusive(
    const FrameId frame) const
{
    std::vector<FrameId> result;

    auto found = frames_.find(frame);
    if (found == frames_.end())
    {
        return result;
    }

    FrameId current = frame;

    while (true)
    {
        result.push_back(current);

        const auto currentRecord =
            frames_.find(current);

        if (currentRecord ==
            frames_.end())
        {
            return {};
        }

        if (!currentRecord->
                second.parent.has_value())
        {
            break;
        }

        current =
            *currentRecord->second.parent;
    }

    return result;
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

    const std::vector<FrameId>
        sourceAncestors =
            AncestorsInclusive(source);
    const std::vector<FrameId>
        targetAncestors =
            AncestorsInclusive(target);

    if (sourceAncestors.empty() ||
        targetAncestors.empty())
    {
        return std::nullopt;
    }

    std::unordered_map<FrameId, std::size_t>
        targetDepth;

    targetDepth.reserve(
        targetAncestors.size());

    for (std::size_t index = 0;
         index < targetAncestors.size();
         ++index)
    {
        targetDepth.emplace(
            targetAncestors[index],
            index);
    }

    std::optional<FrameId>
        commonAncestor;

    std::size_t sourceToCommonCount = 0;
    std::size_t targetToCommonCount = 0;

    for (std::size_t sourceIndex = 0;
         sourceIndex <
            sourceAncestors.size();
         ++sourceIndex)
    {
        const auto match =
            targetDepth.find(
                sourceAncestors[
                    sourceIndex]);

        if (match != targetDepth.end())
        {
            commonAncestor =
                sourceAncestors[
                    sourceIndex];
            sourceToCommonCount =
                sourceIndex;
            targetToCommonCount =
                match->second;
            break;
        }
    }

    if (!commonAncestor.has_value())
    {
        return std::nullopt;
    }

    math::RigidTransformD
        commonFromSource =
            math::IdentityRigidTransformD();

    for (std::size_t index = 0;
         index < sourceToCommonCount;
         ++index)
    {
        const auto record =
            frames_.find(
                sourceAncestors[index]);

        const math::RigidTransformD
            parentFromCurrent =
                record->second.
                    parentFromFrame(
                        atTime);

        commonFromSource =
            math::Compose(
                parentFromCurrent,
                commonFromSource);
    }

    math::RigidTransformD
        commonFromTarget =
            math::IdentityRigidTransformD();

    for (std::size_t index = 0;
         index < targetToCommonCount;
         ++index)
    {
        const auto record =
            frames_.find(
                targetAncestors[index]);

        const math::RigidTransformD
            parentFromCurrent =
                record->second.
                    parentFromFrame(
                        atTime);

        commonFromTarget =
            math::Compose(
                parentFromCurrent,
                commonFromTarget);
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
