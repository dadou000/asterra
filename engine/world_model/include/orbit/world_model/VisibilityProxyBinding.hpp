#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>
#include <vector>

namespace orbit::world_model
{
enum class ResolvedVisibilityProxyShape : u8
{
    Sphere,
    Box
};

struct ResolvedVisibilityProxy
{
    scene::ObjectId object{};
    ResolvedVisibilityProxyShape shape{
        ResolvedVisibilityProxyShape::Sphere};

    math::Double3 positionMeters{};
    math::Double3 eulerDegrees{};

    f64 radiusMeters{0.5};
    math::Double3 halfExtentsMeters{
        0.5, 0.5, 0.5};

    u32 materialId{0U};
    u32 instanceId{0U};
    f32 maximumApproximationErrorMeters{0.25F};
    bool dynamic{false};
};

[[nodiscard]] std::vector<ResolvedVisibilityProxy>
ResolveVisibilityProxies(
    const scene::ObjectStore& objects,
    std::optional<scene::ObjectId> root = std::nullopt);
} // namespace orbit::world_model
