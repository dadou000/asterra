#pragma once

#include <orbit/celestial_giants/GiantAppearance.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>

namespace orbit::world_model
{
struct ResolvedGiantAppearance
{
    scene::ObjectId body{};
    scene::ObjectId capability{};
    celestial_giants::GiantAppearanceParameters parameters{};
    u64 fingerprint{0};
};

[[nodiscard]] std::optional<ResolvedGiantAppearance>
ResolveGiantAppearance(
    const scene::ObjectStore& objects,
    scene::ObjectId body);
} // namespace orbit::world_model
