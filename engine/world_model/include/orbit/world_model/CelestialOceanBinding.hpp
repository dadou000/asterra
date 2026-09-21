#pragma once

#include <orbit/celestial_ocean/OceanOptics.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>

namespace orbit::world_model
{
struct ResolvedOceanBody
{
    scene::ObjectId body{};
    scene::ObjectId oceanCapability{};
    std::optional<scene::ObjectId> sourceObject;
    celestial_ocean::OceanOpticalParameters optical{};
};

[[nodiscard]] std::optional<ResolvedOceanBody>
ResolveOceanBody(
    const scene::ObjectStore& objects,
    scene::ObjectId body);
} // namespace orbit::world_model
