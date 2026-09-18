#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

namespace orbit::terrain_material_column
{
// Snapshot of the M02 substrate record corresponding to the M08 column.
// This is intentionally geology-only: biome state is not an input to physical
// exposed-material resolution.
struct GeologySample
{
    terrain_geology::RockTypeId bedrockMaterial{};

    f32 hardness{0.0F};
    f32 cohesion{0.0F};
    f32 hydraulicErodibility{0.0F};
    f32 aeolianErodibility{0.0F};
    f32 permeability{0.0F};
    f32 chemicalWeatherability{0.0F};
    f32 fractureTendency{0.0F};
    f32 densityKgPerCubicMeter{0.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

// Process state that may affect how consumers treat the exposed physical
// surface, but never which physical material is on top.
struct ProcessDerivedFields
{
    f32 standingWaterDepthMeters{0.0F};
    f32 snowDepthMeters{0.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ExposedSurfaceState
{
    ExposedSurfaceKind material{
        ExposedSurfaceKind::Bedrock};

    // Geological substrate identity remains available even while covered.
    terrain_geology::RockTypeId substrateRock{};

    // Valid only when material == Bedrock. Loose cover never pretends to be a
    // rock type.
    terrain_geology::RockTypeId exposedRock{};

    // Thickness of the actual exposed loose layer. Zero for bedrock.
    f32 exposedLayerDepthMeters{0.0F};

    f32 surfaceHeightMeters{0.0F};
    f32 moisture{0.0F};

    f32 standingWaterDepthMeters{0.0F};
    f32 snowDepthMeters{0.0F};

    // Intrinsic M02 properties of substrateRock. Consumers can use these when
    // bedrock is exposed without another independently-classified geology path.
    GeologySample geology{};

    [[nodiscard]] bool BedrockExposed() const noexcept;
    [[nodiscard]] bool IsValid() const noexcept;
};

// Resolves the M02 geology sample for the rock identity already stored in M08.
// A missing rock is an invalid physical page, not a biome/material fallback.
[[nodiscard]] GeologySample SampleColumnGeology(
    const MaterialColumnCell& column,
    const terrain_geology::GeologicalMaterialLibrary& geology);

// Canonical M18 resolver. The topmost actual M08 material determines exposure.
// Process fields are forwarded for shared consumers but cannot change the
// physical material class or geological substrate identity.
[[nodiscard]] ExposedSurfaceState ResolveSurface(
    const MaterialColumnCell& column,
    const GeologySample& geology,
    const ProcessDerivedFields& fields = {});
} // namespace orbit::terrain_material_column
