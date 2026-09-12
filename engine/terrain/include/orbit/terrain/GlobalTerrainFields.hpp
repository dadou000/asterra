#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/TerrainFields.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/world/Planet.hpp>

namespace orbit::terrain
{
struct GlobalTerrainFieldDesc
{
    // Zero means derive from the owning terrain source seed.
    u64 seed{0};

    f64 seaLevelMeters{0.0};

    f64 continentalAmplitudeMeters{3'600.0};
    f64 continentalWavelengthMeters{4'800'000.0};
    f64 continentalBiasMeters{-650.0};

    f64 mountainAmplitudeMeters{2'800.0};
    f64 mountainWavelengthMeters{1'500'000.0};

    f64 climateWavelengthMeters{3'200'000.0};
    f64 equatorTemperatureC{31.0};
    f64 poleTemperatureC{-24.0};
    f64 temperatureVariationC{4.0};
    f64 lapseRateCPerKilometer{6.2};
};

struct GlobalTerrainFieldSample
{
    f64 coarseElevationMeters{0.0};
    TerrainClimate climate{};
    BiomeWeights biomes{};
};

class GlobalTerrainFields
{
public:
    GlobalTerrainFields(
        world::PlanetDefinition planet,
        GlobalTerrainFieldDesc desc = {});

    [[nodiscard]] GlobalTerrainFieldSample Sample(
        const TerrainQuery& query) const noexcept;

    [[nodiscard]] const GlobalTerrainFieldDesc&
    Description() const noexcept;

private:
    world::PlanetDefinition planet_;
    GlobalTerrainFieldDesc desc_;
};
} // namespace orbit::terrain
