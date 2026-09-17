#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainPosition.hpp>
#include <orbit/terrain_macro_geology/MacroGeologyField.hpp>
#include <orbit/world/Planet.hpp>

#include <array>

namespace orbit::terrain_relief
{
inline constexpr u32 kMaxReliefOctaves = 12;

struct BaseReliefDesc
{
    // Zero derives from PlanetDefinition::generationSeed.
    u64 seed{0};

    f64 continentalAmplitudeMeters{3'000.0};
    f64 continentalWavelengthMeters{4'800'000.0};
    f64 continentalBiasMeters{-550.0};

    f64 ridgeAmplitudeMeters{900.0};
    f64 ridgeWavelengthMeters{420'000.0};
    u32 ridgeOctaves{5};

    f64 valleyAmplitudeMeters{650.0};
    f64 valleyWavelengthMeters{310'000.0};
    u32 valleyOctaves{5};

    f64 lacunarity{2.0};
    f64 persistence{0.5};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ReliefDerivative
{
    // dh / d(meters east), dh / d(meters north).
    // Height meters divided by horizontal meters, so these are local slopes.
    f64 east{0.0};
    f64 north{0.0};

    [[nodiscard]] math::Double2 AsVector() const noexcept
    {
        return {east, north};
    }
};

struct BaseReliefSample
{
    f64 baseContinentalMeters{0.0};
    f64 ridgeDetailMeters{0.0};
    f64 valleyDetailMeters{0.0};
    f64 heightMeters{0.0};

    // Process-independent shape fields. They are useful to later geology,
    // material and rendering consumers but do not encode erosion authority.
    f64 ridgeSignal{0.0};
    f64 valleySignal{0.0};
    f64 geologicalDetailSignal{0.0};

    ReliefDerivative derivative{};

    // M05 is forwarded as forcing/guidance, never silently converted into
    // final M06 height. M09/M10 remain responsible for equilibrium relief.
    f64 macroUpliftMeters{0.0};
    f64 drainageGuidance{0.0};
    f64 protection{0.0};

    u32 activeDetailBands{0};
};

class BaseReliefField
{
public:
    BaseReliefField(
        world::PlanetDefinition planet,
        const terrain_macro_geology::MacroGeologyField* macroGeology,
        BaseReliefDesc desc = {});

    [[nodiscard]] BaseReliefSample Sample(
        const terrain::PlanetSurfacePosition& position,
        const terrain::TerrainSampleFootprint& footprint) const;

    [[nodiscard]] const BaseReliefDesc&
    Description() const noexcept;

private:
    struct Band
    {
        f64 wavelengthMeters{1.0};
        f64 amplitudeMeters{0.0};
        u64 seed{0};
    };

    world::PlanetDefinition planet_{};
    const terrain_macro_geology::MacroGeologyField* macroGeology_{nullptr};
    BaseReliefDesc desc_{};
    u64 resolvedSeed_{0};

    std::array<Band, kMaxReliefOctaves> ridgeBands_{};
    std::array<Band, kMaxReliefOctaves> valleyBands_{};
};

// Smooth physical anti-aliasing weight used by every M06 procedural band.
// <= 2*footprint: removed. >= 4*footprint: fully represented.
[[nodiscard]] f64 ReliefFrequencyWeight(
    f64 wavelengthMeters,
    f64 footprintMeters) noexcept;
} // namespace orbit::terrain_relief
