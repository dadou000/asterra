#pragma once

#include <orbit/celestial_radiometry/Radiometry.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>

namespace orbit::world_model
{
struct ResolvedRadiativeBody
{
    scene::ObjectId body{};
    scene::ObjectId emitterCapability{};
    std::optional<scene::ObjectId> photosphereCapability;
    celestial_radiometry::RadiativeState radiative{};
};

[[nodiscard]] std::optional<ResolvedRadiativeBody>
ResolveRadiativeBody(
    const scene::ObjectStore& objects,
    scene::ObjectId body);
} // namespace orbit::world_model
