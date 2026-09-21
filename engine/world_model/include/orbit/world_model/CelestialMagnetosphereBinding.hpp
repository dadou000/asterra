#pragma once

#include <orbit/celestial_magnetosphere/Magnetosphere.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>

namespace orbit::world_model
{
struct ResolvedMagnetosphere
{
    scene::ObjectId body{};
    scene::ObjectId capability{};
    celestial_magnetosphere::MagnetosphereParameters parameters{};
    u64 fingerprint{0};
};

[[nodiscard]] std::optional<ResolvedMagnetosphere>
ResolveMagnetosphere(
    const scene::ObjectStore& objects,
    scene::ObjectId body,
    f64 referenceRadiusMeters);
} // namespace orbit::world_model
