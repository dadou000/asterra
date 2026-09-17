#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/surface_authoring/TerrainConstraints.hpp>
#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain/TerrainPosition.hpp>
#include <orbit/world/Planet.hpp>

#include <cstdint>

namespace orbit::terrain_macro_geology
{
struct MacroGeologyDesc
{
    // Zero means derive from PlanetDefinition::generationSeed.
    u64 seed{0};

    // Tectonic convergence is positive uplift. Divergence can create
    // rift/subsidence forcing. Hotspot relief is supplied by the same
    // deterministic tectonic field used by GlobalTerrainFields.
    f64 convergenceUpliftMeters{3'200.0};
    f64 continentalCollisionScale{1.0};
    f64 mixedCollisionScale{0.82};
    f64 oceanicCollisionScale{0.62};
    f64 divergenceSubsidenceMeters{1'200.0};

    // Long-wavelength body-space geological distortion. This modulates the
    // uplift forcing; it is not final terrain detail and therefore does not
    // depend on render/clipmap LOD.
    f64 distortionAmplitude{0.22};
    f64 distortionWavelengthMeters{1'800'000.0};
    u32 distortionOctaves{3};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct MacroGeologyRevisionInputs
{
    // Only authorities that can change the M05 field belong here.
    // M06 fine-detail revisions, camera/cache state, biome and water are
    // deliberately absent.
    u64 geology{0};
    u64 authoring{0};

    [[nodiscard]] bool operator==(
        const MacroGeologyRevisionInputs&) const noexcept = default;
};

[[nodiscard]] u64 MacroGeologyRevisionFingerprint(
    const MacroGeologyRevisionInputs& revisions) noexcept;

struct MacroGeologySample
{
    f64 tectonicUpliftMeters{0.0};
    f64 tectonicSubsidenceMeters{0.0};
    f64 hotspotUpliftMeters{0.0};
    f64 distortionSignal{0.0};

    // Final M05 uplift forcing after M04 authored uplift composition.
    // This is an input to later stream-power equilibrium, not final height.
    f64 upliftMeters{0.0};

    // M04 fields forwarded intact for later macro solvers.
    f64 authoredHeightMeters{0.0};
    math::Double2 gradientGuidance{};
    f64 drainageGuidance{0.0};
    f64 protection{0.0};
};

class MacroGeologyField
{
public:
    MacroGeologyField(
        world::PlanetDefinition planet,
        const terrain::GlobalTerrainFields& globalFields,
        const surface_authoring::TerrainConstraintSet* authoredConstraints,
        MacroGeologyDesc desc = {});

    [[nodiscard]] MacroGeologySample Sample(
        const terrain::PlanetSurfacePosition& position) const;

    [[nodiscard]] const MacroGeologyDesc&
    Description() const noexcept;

private:
    [[nodiscard]] f64 DistortionSignal(
        const math::Double3& unitDirection) const noexcept;

    world::PlanetDefinition planet_{};
    const terrain::GlobalTerrainFields* globalFields_{nullptr};
    const surface_authoring::TerrainConstraintSet* authoredConstraints_{nullptr};
    MacroGeologyDesc desc_{};
    u64 resolvedSeed_{0};
};

// M05 convenience builders. These create normal M04 uplift constraints rather
// than introducing a second authoring representation.
[[nodiscard]] surface_authoring::ScalarTerrainConstraint
MakeMountainBeltConstraint(
    surface_authoring::TerrainConstraintId id,
    surface_authoring::SplineConstraintPrimitive spline,
    f64 upliftMeters,
    f64 opacity = 1.0);

[[nodiscard]] surface_authoring::ScalarTerrainConstraint
MakeBasinConstraint(
    surface_authoring::TerrainConstraintId id,
    surface_authoring::TerrainConstraintPrimitive primitive,
    f64 subsidenceMeters,
    f64 opacity = 1.0);
} // namespace orbit::terrain_macro_geology
