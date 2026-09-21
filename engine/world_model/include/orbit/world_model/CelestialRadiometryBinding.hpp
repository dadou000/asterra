#pragma once

#include <orbit/celestial_radiometry/Radiometry.hpp>
#include <orbit/celestial_stellar/StellarAppearance.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>

namespace orbit::world_model
{
struct ResolvedRadiativeBody
{
    scene::ObjectId body{};
    scene::ObjectId emitterCapability{};
    std::optional<scene::ObjectId> photosphereCapability;
    f64 photosphereRadiusMeters{1.0};
    celestial_radiometry::RadiativeState radiative{};
    celestial_stellar::StellarAppearanceParameters stellarAppearance{};
    math::Double3 stellarColorLinear{1.0, 1.0, 1.0};
    u64 stellarAppearanceFingerprint{0};
};

[[nodiscard]] std::optional<ResolvedRadiativeBody>
ResolveRadiativeBody(
    const scene::ObjectStore& objects,
    scene::ObjectId body);
} // namespace orbit::world_model
