#include <orbit/lighting/SoftwareProxyVisibility.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] math::Double3 Min3(
    const math::Double3& a,
    const math::Double3& b) noexcept
{
    return {
        std::min(a.x, b.x),
        std::min(a.y, b.y),
        std::min(a.z, b.z)
    };
}

[[nodiscard]] math::Double3 Max3(
    const math::Double3& a,
    const math::Double3& b) noexcept
{
    return {
        std::max(a.x, b.x),
        std::max(a.y, b.y),
        std::max(a.z, b.z)
    };
}

[[nodiscard]] f64 Axis(
    const math::Double3& value,
    const u32 axis) noexcept
{
    switch (axis)
    {
    case 0U: return value.x;
    case 1U: return value.y;
    default: return value.z;
    }
}

[[nodiscard]] bool IntersectAabb(
    const math::Double3& origin,
    const math::Double3& direction,
    const math::Double3& minimum,
    const math::Double3& maximum,
    const f64 minDistance,
    const f64 maxDistance,
    f64& outNear) noexcept
{
    f64 nearDistance = minDistance;
    f64 farDistance = maxDistance;

    for (u32 axis = 0U; axis < 3U; ++axis)
    {
        const f64 o = Axis(origin, axis);
        const f64 d = Axis(direction, axis);
        const f64 mn = Axis(minimum, axis);
        const f64 mx = Axis(maximum, axis);

        if (std::abs(d) <= 1.0e-15)
        {
            if (o < mn || o > mx)
            {
                return false;
            }
            continue;
        }

        f64 t0 = (mn - o) / d;
        f64 t1 = (mx - o) / d;

        if (t0 > t1)
        {
            std::swap(t0, t1);
        }

        nearDistance =
            std::max(nearDistance, t0);
        farDistance =
            std::min(farDistance, t1);

        if (farDistance < nearDistance)
        {
            return false;
        }
    }

    outNear = nearDistance;
    return true;
}

struct PrimitiveHit
{
    bool hit{false};
    f64 distance{0.0};
    math::Double3 normal{};
};

[[nodiscard]] PrimitiveHit IntersectSphere(
    const math::Double3& origin,
    const math::Double3& direction,
    const f64 radius,
    const f64 minimumDistance,
    const f64 maximumDistance) noexcept
{
    const f64 b =
        math::Dot(origin, direction);
    const f64 c =
        math::Dot(origin, origin) -
        radius * radius;
    const f64 discriminant =
        b * b - c;

    if (discriminant < 0.0)
    {
        return {};
    }

    const f64 root =
        std::sqrt(
            std::max(
                discriminant,
                0.0));

    const std::array candidates{
        -b - root,
        -b + root
    };

    for (const f64 distance : candidates)
    {
        if (distance >= minimumDistance &&
            distance <= maximumDistance)
        {
            const auto point =
                origin +
                direction * distance;

            return {
                .hit = true,
                .distance = distance,
                .normal =
                    math::Normalize(point)
            };
        }
    }

    return {};
}

[[nodiscard]] PrimitiveHit IntersectBox(
    const math::Double3& origin,
    const math::Double3& direction,
    const math::Double3& halfExtents,
    const f64 minimumDistance,
    const f64 maximumDistance) noexcept
{
    const math::Double3 minimum =
        halfExtents * -1.0;
    const math::Double3 maximum =
        halfExtents;

    f64 nearDistance = minimumDistance;
    f64 farDistance = maximumDistance;
    u32 nearAxis = 0U;
    f64 nearSign = 1.0;

    for (u32 axis = 0U; axis < 3U; ++axis)
    {
        const f64 o = Axis(origin, axis);
        const f64 d = Axis(direction, axis);
        const f64 mn = Axis(minimum, axis);
        const f64 mx = Axis(maximum, axis);

        if (std::abs(d) <= 1.0e-15)
        {
            if (o < mn || o > mx)
            {
                return {};
            }
            continue;
        }

        f64 t0 = (mn - o) / d;
        f64 t1 = (mx - o) / d;
        f64 sign = -1.0;

        if (t0 > t1)
        {
            std::swap(t0, t1);
            sign = 1.0;
        }

        if (t0 > nearDistance)
        {
            nearDistance = t0;
            nearAxis = axis;
            nearSign = sign;
        }

        farDistance =
            std::min(farDistance, t1);

        if (farDistance < nearDistance)
        {
            return {};
        }
    }

    math::Double3 normal{};
    if (nearAxis == 0U) normal.x = nearSign;
    else if (nearAxis == 1U) normal.y = nearSign;
    else normal.z = nearSign;

    return {
        .hit = true,
        .distance = nearDistance,
        .normal = normal
    };
}

[[nodiscard]] f32 ConfidenceForError(
    const f32 nominalErrorMeters,
    const VisibilityQuery& query) noexcept
{
    if (!std::isfinite(nominalErrorMeters) ||
        nominalErrorMeters < 0.0F)
    {
        return 0.0F;
    }

    if (std::isfinite(
            query.requirements.
                maximumNominalErrorMeters) &&
        query.requirements.
                maximumNominalErrorMeters >
            0.0F)
    {
        return std::clamp(
            1.0F -
                nominalErrorMeters /
                    query.requirements.
                        maximumNominalErrorMeters,
            0.0F,
            1.0F);
    }

    return
        1.0F /
        (1.0F +
         nominalErrorMeters);
}
} // namespace

void SoftwareProxyScene::Rebuild(
    const std::span<const VisibilityProxy> proxies,
    const frames::FrameId targetFrame,
    const frames::FrameGraph& frames,
    const time::SimulationTime atTime)
{
    frame_ = targetFrame;
    proxies_.clear();
    indices_.clear();
    nodes_.clear();
    stats_ = {};

    for (const auto& proxy : proxies)
    {
        if (!proxy.frame ||
            proxy.stableId == 0U)
        {
            continue;
        }

        const auto targetFromFrame =
            frames.ResolveTransform(
                proxy.frame,
                targetFrame,
                atTime);

        if (!targetFromFrame.has_value())
        {
            continue;
        }

        const math::RigidTransformD
            targetFromProxy =
                math::Compose(
                    *targetFromFrame,
                    proxy.frameFromProxy);

        math::Double3 minimum{
            std::numeric_limits<f64>::infinity(),
            std::numeric_limits<f64>::infinity(),
            std::numeric_limits<f64>::infinity()
        };
        math::Double3 maximum{
            -std::numeric_limits<f64>::infinity(),
            -std::numeric_limits<f64>::infinity(),
            -std::numeric_limits<f64>::infinity()
        };

        if (proxy.shape ==
            VisibilityProxyShape::Sphere)
        {
            const auto center =
                targetFromProxy.translation;
            const f64 radius =
                std::max(
                    proxy.sphereRadiusMeters,
                    0.0);

            minimum = {
                center.x - radius,
                center.y - radius,
                center.z - radius
            };
            maximum = {
                center.x + radius,
                center.y + radius,
                center.z + radius
            };
        }
        else
        {
            const auto e =
                proxy.boxHalfExtentsMeters;

            for (u32 corner = 0U;
                 corner < 8U;
                 ++corner)
            {
                const math::Double3 local{
                    (corner & 1U) ? e.x : -e.x,
                    (corner & 2U) ? e.y : -e.y,
                    (corner & 4U) ? e.z : -e.z
                };

                const auto point =
                    math::TransformPoint(
                        targetFromProxy,
                        local);

                minimum =
                    Min3(minimum, point);
                maximum =
                    Max3(maximum, point);
            }
        }

        proxies_.push_back({
            .source = proxy,
            .frameFromProxy =
                targetFromProxy,
            .boundsMinimum = minimum,
            .boundsMaximum = maximum,
            .centroid =
                (minimum + maximum) *
                0.5
        });

        ++stats_.proxyCount;
        if (proxy.dynamic)
        {
            ++stats_.dynamicProxyCount;
        }

        stats_.maximumNominalErrorMeters =
            std::max(
                stats_.maximumNominalErrorMeters,
                std::max(
                    proxy.nominalErrorMeters,
                    0.0F));
    }

    indices_.resize(
        proxies_.size());

    std::iota(
        indices_.begin(),
        indices_.end(),
        0U);

    if (!indices_.empty())
    {
        BuildNode(
            0U,
            static_cast<u32>(
                indices_.size()));
    }

    stats_.nodeCount =
        static_cast<u32>(
            nodes_.size());
}

frames::FrameId
SoftwareProxyScene::Frame() const noexcept
{
    return frame_;
}

const SoftwareProxySceneStats&
SoftwareProxyScene::Stats() const noexcept
{
    return stats_;
}

u32 SoftwareProxyScene::BuildNode(
    const u32 first,
    const u32 count)
{
    Node node;
    node.minimum = {
        std::numeric_limits<f64>::infinity(),
        std::numeric_limits<f64>::infinity(),
        std::numeric_limits<f64>::infinity()
    };
    node.maximum = {
        -std::numeric_limits<f64>::infinity(),
        -std::numeric_limits<f64>::infinity(),
        -std::numeric_limits<f64>::infinity()
    };

    math::Double3 centroidMinimum =
        node.minimum;
    math::Double3 centroidMaximum =
        node.maximum;

    for (u32 offset = 0U;
         offset < count;
         ++offset)
    {
        const auto& proxy =
            proxies_[
                indices_[first + offset]];

        node.minimum =
            Min3(
                node.minimum,
                proxy.boundsMinimum);
        node.maximum =
            Max3(
                node.maximum,
                proxy.boundsMaximum);

        centroidMinimum =
            Min3(
                centroidMinimum,
                proxy.centroid);
        centroidMaximum =
            Max3(
                centroidMaximum,
                proxy.centroid);
    }

    const u32 nodeIndex =
        static_cast<u32>(
            nodes_.size());

    nodes_.push_back(node);

    if (count <= 4U)
    {
        nodes_[nodeIndex].first = first;
        nodes_[nodeIndex].count = count;
        return nodeIndex;
    }

    const math::Double3 extent =
        centroidMaximum -
        centroidMinimum;

    u32 axis = 0U;
    if (extent.y > extent.x &&
        extent.y >= extent.z)
    {
        axis = 1U;
    }
    else if (extent.z > extent.x &&
             extent.z > extent.y)
    {
        axis = 2U;
    }

    const u32 middle =
        first + count / 2U;

    std::nth_element(
        indices_.begin() + first,
        indices_.begin() + middle,
        indices_.begin() + first + count,
        [&](const u32 a,
            const u32 b)
        {
            return
                Axis(
                    proxies_[a].centroid,
                    axis) <
                Axis(
                    proxies_[b].centroid,
                    axis);
        });

    const u32 left =
        BuildNode(
            first,
            middle - first);

    const u32 right =
        BuildNode(
            middle,
            first + count - middle);

    nodes_[nodeIndex].left = left;
    nodes_[nodeIndex].right = right;

    return nodeIndex;
}

SoftwareProxyVisibilityProvider::
SoftwareProxyVisibilityProvider(
    const SoftwareProxyScene& scene,
    SoftwareProxyTraversalBudget budget)
    : scene_(&scene),
      budget_(budget),
      desc_({
          .providerId =
              0x534f465450524f58ULL,
          .name =
              "Software Proxy",
          .kind =
              VisibilityBackendKind::
                  SoftwareProxy,
          .capabilities =
              VisibilityCapability::Offscreen |
              VisibilityCapability::DynamicGeometry |
              VisibilityCapability::SurfaceMaterial,
          .nominalErrorMeters =
              scene.Stats().
                  maximumNominalErrorMeters,
          .priority = 10
      })
{
    budget_.maximumNodeVisits =
        std::max(
            budget_.maximumNodeVisits,
            1U);
    budget_.maximumPrimitiveTests =
        std::max(
            budget_.maximumPrimitiveTests,
            1U);
}

const VisibilityProviderDesc&
SoftwareProxyVisibilityProvider::
Description() const noexcept
{
    return desc_;
}

bool SoftwareProxyVisibilityProvider::
SupportsPurpose(
    const VisibilityPurpose purpose) const noexcept
{
    switch (purpose)
    {
    case VisibilityPurpose::DiffuseGi:
    case VisibilityPurpose::Reflection:
    case VisibilityPurpose::Shadow:
    case VisibilityPurpose::SkyVisibility:
    case VisibilityPurpose::ProbeUpdate:
    case VisibilityPurpose::Diagnostic:
        return true;
    }

    return false;
}

VisibilityResult
SoftwareProxyVisibilityProvider::Trace(
    const VisibilityQuery& query)
{
    if (scene_ == nullptr ||
        query.frame != scene_->frame_ ||
        scene_->nodes_.empty())
    {
        return {};
    }

    const math::Double3 origin =
        query.originInFrameMeters;
    const math::Double3 direction =
        math::Normalize({
            static_cast<f64>(query.direction.x),
            static_cast<f64>(query.direction.y),
            static_cast<f64>(query.direction.z)
        });

    std::array<u32, 256> stack{};
    u32 stackSize = 1U;
    stack[0] = 0U;

    u32 nodeVisits = 0U;
    u32 primitiveTests = 0U;

    f64 closestDistance =
        static_cast<f64>(
            query.maximumDistanceMeters);

    const SoftwareProxyScene::ResolvedProxy*
        closestProxy = nullptr;
    math::Double3 closestNormal{};

    bool budgetExhausted = false;

    while (stackSize > 0U)
    {
        if (nodeVisits >=
            budget_.maximumNodeVisits)
        {
            budgetExhausted = true;
            break;
        }

        const u32 nodeIndex =
            stack[--stackSize];

        ++nodeVisits;

        const auto& node =
            scene_->nodes_[nodeIndex];

        f64 nodeNear = 0.0;

        if (!IntersectAabb(
                origin,
                direction,
                node.minimum,
                node.maximum,
                query.minimumDistanceMeters,
                closestDistance,
                nodeNear))
        {
            continue;
        }

        if (node.IsLeaf())
        {
            for (u32 offset = 0U;
                 offset < node.count;
                 ++offset)
            {
                if (primitiveTests >=
                    budget_.
                        maximumPrimitiveTests)
                {
                    budgetExhausted = true;
                    break;
                }

                ++primitiveTests;

                const auto& proxy =
                    scene_->proxies_[
                        scene_->indices_[
                            node.first +
                            offset]];

                if (query.body &&
                    proxy.source.body &&
                    query.body !=
                        proxy.source.body)
                {
                    continue;
                }

                const auto proxyFromFrame =
                    math::Inverse(
                        proxy.frameFromProxy);

                const math::Double3
                    localOrigin =
                        math::TransformPoint(
                            proxyFromFrame,
                            origin);

                const math::Double3
                    localDirection =
                        math::Normalize(
                            math::TransformVector(
                                proxyFromFrame.
                                    rotation,
                                direction));

                PrimitiveHit hit;

                if (proxy.source.shape ==
                    VisibilityProxyShape::Sphere)
                {
                    hit =
                        IntersectSphere(
                            localOrigin,
                            localDirection,
                            std::max(
                                proxy.source.
                                    sphereRadiusMeters,
                                0.0),
                            query.
                                minimumDistanceMeters,
                            closestDistance);
                }
                else
                {
                    hit =
                        IntersectBox(
                            localOrigin,
                            localDirection,
                            proxy.source.
                                boxHalfExtentsMeters,
                            query.
                                minimumDistanceMeters,
                            closestDistance);
                }

                if (!hit.hit ||
                    hit.distance >=
                        closestDistance)
                {
                    continue;
                }

                closestDistance =
                    hit.distance;
                closestProxy =
                    &proxy;

                closestNormal =
                    math::Normalize(
                        math::TransformVector(
                            proxy.
                                frameFromProxy.
                                rotation,
                            hit.normal));
            }

            if (budgetExhausted)
            {
                break;
            }

            continue;
        }

        if (stackSize + 2U >
            stack.size())
        {
            budgetExhausted = true;
            break;
        }

        stack[stackSize++] =
            node.left;
        stack[stackSize++] =
            node.right;
    }

    if (closestProxy != nullptr)
    {
        const auto position =
            origin +
            direction *
                closestDistance;

        const math::Float3 normal{
            static_cast<f32>(
                closestNormal.x),
            static_cast<f32>(
                closestNormal.y),
            static_cast<f32>(
                closestNormal.z)
        };

        const f32 confidence =
            ConfidenceForError(
                closestProxy->source.
                    nominalErrorMeters,
                query);

        return {
            .resolution =
                VisibilityResolution::Hit,
            .hit = {
                .distanceMeters =
                    static_cast<f32>(
                        closestDistance),
                .positionInFrameMeters =
                    position,
                .geometricNormal =
                    normal,
                .shadingNormal =
                    normal,
                .materialId =
                    closestProxy->source.
                        materialId,
                .instanceId =
                    closestProxy->source.
                        instanceId
            },
            .confidence = confidence,
            .terminal = !budgetExhausted
        };
    }

    if (budgetExhausted)
    {
        return {
            .resolution =
                VisibilityResolution::
                    Unresolved,
            .confidence = 0.0F,
            .terminal = false
        };
    }

    return {
        .resolution =
            VisibilityResolution::Miss,
        .confidence = 1.0F,
        .terminal = true
    };
}
} // namespace orbit::lighting
