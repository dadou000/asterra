#pragma once

#include <orbit/celestial_clouds/CloudField.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>
#include <vector>

namespace orbit::world_model
{
struct ResolvedCloudLayer
{
    scene::ObjectId body{};
    scene::ObjectId capability{};
    std::optional<scene::ObjectId> sourceObject;
    celestial_clouds::CloudLayerParameters parameters{};
};

[[nodiscard]] std::vector<ResolvedCloudLayer>
ResolveCloudLayers(
    const scene::ObjectStore& objects,
    scene::ObjectId body);
} // namespace orbit::world_model
