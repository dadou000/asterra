#pragma once

#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/world/Planet.hpp>

#include <array>

namespace orbit::terrain
{
struct MountainTerrainDesc
{
    f64 reliefMeters{11'000.0};
    f64 wavelengthMeters{24'000.0};
    u32 octaves{8};
    f64 warpWavelengthMeters{70'000.0};
    f64 warpAmplitudeMeters{9'000.0};
};

struct AnalyticTerrainDesc
{
    u64 seed{0x41535445525241ULL};

    // Regional landform band layered on top of the global fields.
    f64 macroAmplitudeMeters{1'200.0};
    f64 macroWavelengthMeters{800'000.0};

    // Local terrain detail. Octaves are footprint-filtered.
    f64 detailAmplitudeMeters{320.0};
    f64 detailWavelengthMeters{40'000.0};
    u32 detailOctaves{10};

    GlobalTerrainFieldDesc global{};
    MountainTerrainDesc mountains{};
    // Mountain relief and local detail share the available elevation headroom.
    f64 maximumElevationAboveSeaLevelMeters{8'000.0};
};

class AnalyticTerrainSource final : public TerrainSource
{
public:
    explicit AnalyticTerrainSource(
        world::PlanetDefinition planet,
        AnalyticTerrainDesc desc = {});

    [[nodiscard]] TerrainSample Sample(
        const TerrainQuery& query) const noexcept override;

    [[nodiscard]] u64 Revision() const noexcept override;

    // Exposes the coarse whole-planet fields (continents, coarse
    // mountain ridges, climate, plate/hotspot tectonic data) this
    // source is itself built on. Used by callers that need cheap
    // planet-wide surveys -- e.g. the map renderer's per-layer
    // textures, or a rain-shadow-style upwind probe -- without paying
    // for this source's own detail octaves/fine ridge noise.
    [[nodiscard]] const GlobalTerrainFields& GlobalFields() const noexcept
    {
        return globalFields_;
    }

    // The exact (already-resolved) recipe this instance was built from --
    // e.g. a GPU field generator built from this source needs these
    // scalar parameters verbatim, the same way it needs GlobalFields()'s
    // plates/hotspots verbatim (see GpuTectonicPlate's comment).
    [[nodiscard]] const AnalyticTerrainDesc& Description() const noexcept
    {
        return desc_;
    }

private:
    struct NoiseOctave
    {
        f64 frequency{0.0};
        f64 wavelengthMeters{0.0};
        f64 amplitude{0.0};
        u64 seed{0};
    };

    [[nodiscard]] f64 MountainShape(
        const math::Double3& direction,
        f64 footprintMeters) const noexcept;

    [[nodiscard]] f64 LimitElevation(f64 elevationMeters) const noexcept;

    world::PlanetDefinition planet_;
    AnalyticTerrainDesc desc_;
    GlobalTerrainFields globalFields_;
    std::array<NoiseOctave, 16> detailBands_{};
    std::array<NoiseOctave, 16> mountainBands_{};
    f64 mountainNormalization_{1.0};
    f64 warpFrequency_{0.0};
    f64 warpFootprintScale_{1.0};
    u64 revision_{0};
};
} // namespace orbit::terrain
