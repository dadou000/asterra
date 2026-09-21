#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>
#include <vector>

namespace orbit::world_model
{
enum class AuthoredLightKind : u8
{
    Point,
    Spot
};

struct AuthoredLocalLight
{
    scene::ObjectId object{};
    AuthoredLightKind kind{AuthoredLightKind::Point};
    math::Double3 positionMeters{};
    math::Double3 direction{0.0, -1.0, 0.0};
    math::Double3 colorLinear{1.0, 1.0, 1.0};
    f64 luminousFluxLumens{0.0};
    f64 rangeMeters{0.0};
    f64 innerConeDegrees{0.0};
    f64 outerConeDegrees{0.0};
};

[[nodiscard]] std::vector<AuthoredLocalLight>
ResolveAuthoredLocalLights(
    const scene::ObjectStore& objects,
    std::optional<scene::ObjectId> root = std::nullopt);
} // namespace orbit::world_model
