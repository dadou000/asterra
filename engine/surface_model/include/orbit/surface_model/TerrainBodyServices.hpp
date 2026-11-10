#pragma once

#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain/BakedRivers.hpp>
#include <orbit/terrain/BakedGeology.hpp>
#include <orbit/terrain/BakedTectonics.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_erosion/AeolianErosion.hpp>
#include <orbit/terrain_erosion/GlacialErosion.hpp>
#include <orbit/terrain_erosion/HydraulicErosion.hpp>
#include <orbit/terrain_erosion/RiverNetwork.hpp>
#include <orbit/terrain_erosion/StreamPowerErosion.hpp>
#include <orbit/terrain_erosion/ThermalErosion.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_geology/Stratigraphy.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/terrain_water/CoastalProcess.hpp>
#include <orbit/terrain_water/WaterService.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <memory>
#include <optional>

namespace orbit::surface_model
{
// Runtime owner for the implemented physical terrain process configuration of
// one rocky body. The process state itself remains in the canonical M08/M14
// physical products; these configs are service-level solver policy only.
// Planet bake policy for one body: how fine the baked rasters are and whether
// a changed recipe rebakes by itself once it stops changing.
struct PlanetBakeSettings
{
    u32 resolution{256};
    bool autoRebake{true};

    [[nodiscard]] bool IsValid() const noexcept
    {
        return resolution >= 16U && resolution <= 2048U;
    }
};

struct TerrainProcessService
{
    PlanetBakeSettings bake{};

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

    // Unique for every TerrainBodyServices ever constructed in the process
    // (a move keeps it with the object). A BodyId is not enough to know that
    // two lookups reached the same services: a world switch or a failed
    // recomposition destroys the services, and the next ones for the same
    // BodyId own a different GPU terrain cache. Holders of a pointer into the
    // services (the physical page service keeps the cache) compare this to
    // notice they went stale instead of dereferencing a freed cache.
    [[nodiscard]] u64 InstanceId() const noexcept;

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

    // What the current terrain source was composed from. The baked planet
    // structure must match this recipe; the bake service compares against it.
    struct BakeRecipe
    {
        world::PlanetDefinition planet{};
        terrain::AnalyticTerrainDesc desc{};
    };
    [[nodiscard]] const std::optional<BakeRecipe>& Recipe() const noexcept;
    void SetRecipe(BakeRecipe recipe);

    // Baked tectonic rasters installed by the bake service; null until a bake
    // exists. The terrain source reads them when it is composed.
    [[nodiscard]] const std::shared_ptr<const terrain::BakedTectonicRasters>&
    TectonicBake() const noexcept;
    void SetTectonicBake(
        std::shared_ptr<const terrain::BakedTectonicRasters> bake) noexcept;

    // Baked river graph installed by the bake service; the terrain source cuts
    // channels from it instead of building drainage while it generates.
    [[nodiscard]] const std::shared_ptr<const terrain::BakedRiverNetwork>&
    RiverBake() const noexcept;
    void SetRiverBake(
        std::shared_ptr<const terrain::BakedRiverNetwork> bake) noexcept;

    [[nodiscard]] const std::shared_ptr<const terrain::BakedGeologyRasters>&
    GeologicalBake() const noexcept;
    void SetGeologicalBake(
        std::shared_ptr<const terrain::BakedGeologyRasters> bake) noexcept;

    [[nodiscard]] const std::shared_ptr<const terrain_geology::CompiledStratigraphyProfile>&
    Stratigraphy() const noexcept;
    void SetStratigraphyProfile(
        std::optional<terrain_geology::StratigraphyProfile> profile);

    [[nodiscard]] bool IsValid() const noexcept;

private:
    std::optional<BakeRecipe> recipe_;
    std::shared_ptr<const terrain::BakedTectonicRasters> tectonicBake_;
    std::shared_ptr<const terrain::BakedRiverNetwork> riverBake_;
    std::shared_ptr<const terrain::BakedGeologyRasters> geologicalBake_;
    std::shared_ptr<const terrain_geology::CompiledStratigraphyProfile> stratigraphy_;
    universe::BodyId body_{};
    u64 instanceId_{0U};
    terrain_geology::GeologicalMaterialLibrary geology_{};
    terrain_geology::RockTypeId defaultBedrock_{
        terrain_geology::reference_rock::Basalt};
    TerrainProcessService processes_{};
    terrain_biome::BiomeService biomes_;
    terrain_water::WaterService water_;
    terrain_gpu::PersistentGpuTerrainCache cache_;
};
} // namespace orbit::surface_model
