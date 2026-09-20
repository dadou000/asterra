#pragma once

#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_erosion/AeolianErosion.hpp>
#include <orbit/terrain_erosion/GlacialErosion.hpp>
#include <orbit/terrain_erosion/HydraulicErosion.hpp>
#include <orbit/terrain_erosion/RiverNetwork.hpp>
#include <orbit/terrain_erosion/StreamPowerErosion.hpp>
#include <orbit/terrain_erosion/ThermalErosion.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/terrain_water/CoastalProcess.hpp>
#include <orbit/terrain_water/WaterService.hpp>
#include <orbit/universe/BodyRegistry.hpp>

namespace orbit::surface_model
{
// Runtime owner for the implemented physical terrain process configuration of
// one rocky body. The process state itself remains in the canonical M08/M14
// physical products; these configs are service-level solver policy only.
struct TerrainProcessService
{
    bool streamPowerEnabled{true};
    terrain_erosion::StreamPowerErosionConfig streamPower{};

    bool hydraulicEnabled{true};
    terrain_erosion::HydraulicErosionConfig hydraulic{};

    bool thermalEnabled{true};
    terrain_erosion::ThermalErosionConfig thermal{};

    bool aeolianEnabled{true};
    terrain_erosion::AeolianErosionConfig aeolian{};

    bool glacialEnabled{true};
    terrain_erosion::GlacialErosionConfig glacial{};

    bool riversEnabled{true};
    terrain_erosion::RiverNetworkConfig rivers{};

    terrain_water::CoastalProcessConfig coastal{};

    [[nodiscard]] bool IsValid() const noexcept;
};

// One lifetime root for the V0.0.4 terrain services attached to a rocky body.
// Authored semantic data remains authoritative; the cache and process products
// are derived runtime state and are intentionally recreated after save/load.
class TerrainBodyServices
{
public:
    explicit TerrainBodyServices(
        universe::BodyId body,
        terrain_gpu::PersistentGpuTerrainCacheConfig cacheConfig = {});

    TerrainBodyServices(const TerrainBodyServices&) = delete;
    TerrainBodyServices& operator=(const TerrainBodyServices&) = delete;
    TerrainBodyServices(TerrainBodyServices&&) noexcept = default;
    TerrainBodyServices& operator=(TerrainBodyServices&&) noexcept = default;

    [[nodiscard]] universe::BodyId Body() const noexcept;

    [[nodiscard]] terrain_geology::GeologicalMaterialLibrary& Geology() noexcept;
    [[nodiscard]] const terrain_geology::GeologicalMaterialLibrary& Geology() const noexcept;

    // Stable physical identity used when no authored geology asset overrides
    // the substrate selection. Coefficients still come from authored M02
    // GeologicalMaterial records rather than from renderer constants.
    [[nodiscard]] terrain_geology::RockTypeId DefaultBedrock() const noexcept;
    void SetDefaultBedrock(terrain_geology::RockTypeId rock);

    [[nodiscard]] TerrainProcessService& Processes() noexcept;
    [[nodiscard]] const TerrainProcessService& Processes() const noexcept;

    [[nodiscard]] terrain_biome::BiomeService& Biomes() noexcept;
    [[nodiscard]] const terrain_biome::BiomeService& Biomes() const noexcept;

    [[nodiscard]] terrain_water::WaterService& Water() noexcept;
    [[nodiscard]] const terrain_water::WaterService& Water() const noexcept;

    [[nodiscard]] terrain_gpu::PersistentGpuTerrainCache& Cache() noexcept;
    [[nodiscard]] const terrain_gpu::PersistentGpuTerrainCache& Cache() const noexcept;

    [[nodiscard]] bool IsValid() const noexcept;

private:
    universe::BodyId body_{};
    terrain_geology::GeologicalMaterialLibrary geology_{};
    terrain_geology::RockTypeId defaultBedrock_{
        terrain_geology::reference_rock::Basalt};
    TerrainProcessService processes_{};
    terrain_biome::BiomeService biomes_;
    terrain_water::WaterService water_;
    terrain_gpu::PersistentGpuTerrainCache cache_;
};
} // namespace orbit::surface_model
