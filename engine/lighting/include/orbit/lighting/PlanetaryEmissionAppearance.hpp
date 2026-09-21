#pragma once

#include <orbit/celestial_appearance/PlanetaryAppearance.hpp>
#include <orbit/lighting/PlanetaryEmissionField.hpp>

namespace orbit::lighting
{
void ApplyPlanetaryEmissionField(
    celestial_appearance::PlanetaryAppearanceProduct& appearance,
    const PlanetaryEmissionField& field,
    f64 referenceRadiusMeters,
    u32 level,
    f32 intensityScale = 1.0F);
} // namespace orbit::lighting
