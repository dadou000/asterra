#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/math/RigidTransform.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace orbit::frames
{
struct FrameIdTag;
using FrameId = core::StrongId<FrameIdTag>;

struct FramePoint
{
    FrameId frame{};
    math::Double3 localMeters{};
};

using FrameTransformProvider =
    std::function<math::RigidTransformD(
        time::SimulationTime)>;

struct FrameDefinition
{
    FrameId id{};
    std::optional<FrameId> parent;
    FrameTransformProvider parentFromFrame;
};

// Sparse, CPU-authoritative hierarchy of reference frames. A frame stores
// only its transform relative to its parent. Pairwise resolution walks to
// the lowest common ancestor instead of converting through a potentially
// astronomical root, preserving local precision for nearby objects.
class FrameGraph
{
public:
    FrameGraph() = default;

    [[nodiscard]] FrameId CreateRoot();

    [[nodiscard]] FrameId CreateRoot(
        FrameId id);

    [[nodiscard]] FrameId CreateFrame(
        FrameId parent,
        FrameTransformProvider parentFromFrame);

    [[nodiscard]] FrameId CreateFrame(
        FrameId id,
        FrameId parent,
        FrameTransformProvider parentFromFrame);

    void AddFrame(FrameDefinition definition);

    [[nodiscard]] bool Contains(
        FrameId frame) const noexcept;

    [[nodiscard]] std::optional<FrameId>
    Parent(FrameId frame) const noexcept;

    [[nodiscard]] std::optional<math::RigidTransformD>
    ResolveTransform(
        FrameId source,
        FrameId target,
        time::SimulationTime atTime) const;

    [[nodiscard]] std::optional<FramePoint>
    TransformPoint(
        const FramePoint& point,
        FrameId target,
        time::SimulationTime atTime) const;

    [[nodiscard]] std::optional<math::Float3>
    ToCameraRelative(
        const FramePoint& point,
        const FramePoint& cameraOrigin,
        time::SimulationTime atTime) const;

private:
    struct FrameRecord
    {
        std::optional<FrameId> parent;
        FrameTransformProvider parentFromFrame;
    };

    [[nodiscard]] std::vector<FrameId>
    AncestorsInclusive(FrameId frame) const;

    std::unordered_map<FrameId, FrameRecord> frames_;
};
} // namespace orbit::frames
