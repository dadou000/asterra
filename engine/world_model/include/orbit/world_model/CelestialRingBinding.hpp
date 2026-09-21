#pragma once

#include <orbit/celestial_rings/RingSystem.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>

namespace orbit::world_model
{
struct ResolvedRingSystem
{
    scene::ObjectId body{};
    scene::ObjectId ringSystem{};
    celestial_rings::RingSystem parameters{};
};

[[nodiscard]] std::optional<ResolvedRingSystem>
ResolveRingSystem(
    const scene::ObjectStore& objects,
    scene::ObjectId body);
} // namespace orbit::world_model
