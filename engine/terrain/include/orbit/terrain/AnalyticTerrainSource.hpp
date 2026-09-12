#pragma once

#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/world/Planet.hpp>

namespace orbit::terrain
{
struct AnalyticTerrainDesc
{
    u64 seed{0x41535445525241ULL};

    // Regional landform band layered on top of the global fields.
    f64 macroAmplitudeMeters{4'500.0};
    f64 macroWavelengthMeters{900'000.0};

    // Local terrain detail. Octaves are footprint-filtered.
    f64 detailAmplitudeMeters{1'200.0};
    f64 detailWavelengthMeters{120'000.0};
    u32 detailOctaves{8};

    GlobalTerrainFieldDesc global{};
};

class AnalyticTerrainSource final : public TerrainSource
{
public:
    explicit AnalyticTerrainSource(
        world::PlanetDefinition planet,
        AnalyticTerrainDesc desc = {});

    [[nodiscard]] TerrainSample Sample(
        const TerrainQuery& query) const noexcept override;

private:
    world::PlanetDefinition planet_;
    AnalyticTerrainDesc desc_;
    GlobalTerrainFields globalFields_;
};
} // namespace orbit::terrain
