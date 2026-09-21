#pragma once

#include <orbit/celestial_atmosphere/Atmosphere.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>

namespace orbit::world_model
{
struct ResolvedAtmosphereBody
{
    scene::ObjectId body{};
    scene::ObjectId atmosphereCapability{};
    celestial_atmosphere::AtmosphereParameters parameters{};
};

[[nodiscard]] std::optional<ResolvedAtmosphereBody>
ResolveAtmosphereBody(
    const scene::ObjectStore& objects,
    scene::ObjectId body);
} // namespace orbit::world_model
