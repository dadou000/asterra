#pragma once

#include <orbit/celestial_small_bodies/SmallBodyAppearance.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>

namespace orbit::world_model
{
struct ResolvedSmallBodyAppearance
{
    scene::ObjectId body{};
    scene::ObjectId capability{};
    celestial_small_bodies::SmallBodyParameters parameters{};
    u64 fingerprint{0};
};

[[nodiscard]] std::optional<ResolvedSmallBodyAppearance>
ResolveSmallBodyAppearance(
    const scene::ObjectStore& objects,
    scene::ObjectId body);
} // namespace orbit::world_model
