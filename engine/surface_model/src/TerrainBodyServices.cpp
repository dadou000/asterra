#include <orbit/surface_model/TerrainBodyServices.hpp>

#include <atomic>
#include <stdexcept>

namespace orbit::surface_model
{
bool TerrainProcessService::IsValid() const noexcept
{
    return
        bake.IsValid() &&
        streamPower.IsValid() &&
        hydraulic.IsValid() &&
        thermal.IsValid() &&
        aeolian.IsValid() &&
        glacial.IsValid() &&
        rivers.IsValid() &&
        coastal.IsValid();
}

namespace
{
[[nodiscard]] u64 NextInstanceId() noexcept
{
    static std::atomic<u64> next{1U};
    return next.fetch_add(1U, std::memory_order_relaxed);
}
} // namespace

TerrainBodyServices::TerrainBodyServices(
    const universe::BodyId body,
    const terrain_gpu::PersistentGpuTerrainCacheConfig cacheConfig)
    : body_(body),
      instanceId_(NextInstanceId()),
      geology_(
          terrain_geology::
              LoadReferenceGeologicalMaterialLibrary()),
      biomes_(body),
      water_(body),
      cache_(cacheConfig)
{
    if (!body_.IsValid())
    {
        throw std::invalid_argument(
            "TerrainBodyServices requires a valid rocky-body identity.");
    }

    if (!processes_.IsValid())
    {
        throw std::logic_error(
            "Default V0.0.4 terrain process service configuration is invalid.");
    }
}

universe::BodyId TerrainBodyServices::Body() const noexcept
{
    return body_;
}

u64 TerrainBodyServices::InstanceId() const noexcept
{
    return instanceId_;
}

terrain_geology::GeologicalMaterialLibrary&
TerrainBodyServices::Geology() noexcept
{
    return geology_;
}

const terrain_geology::GeologicalMaterialLibrary&
TerrainBodyServices::Geology() const noexcept
{
    return geology_;
}

terrain_geology::RockTypeId
TerrainBodyServices::DefaultBedrock() const noexcept
{
    return defaultBedrock_;
}

void TerrainBodyServices::SetDefaultBedrock(
    const terrain_geology::RockTypeId rock)
{
    if (!rock.IsValid())
    {
        throw std::invalid_argument(
            "Default terrain bedrock identity must be valid.");
    }

    defaultBedrock_ = rock;
}

TerrainProcessService&
TerrainBodyServices::Processes() noexcept
{
    return processes_;
}

const TerrainProcessService&
TerrainBodyServices::Processes() const noexcept
{
    return processes_;
}

terrain_biome::BiomeService&
TerrainBodyServices::Biomes() noexcept
{
    return biomes_;
}

const terrain_biome::BiomeService&
TerrainBodyServices::Biomes() const noexcept
{
    return biomes_;
}

terrain_water::WaterService& TerrainBodyServices::Water() noexcept
{
    return water_;
}

const terrain_water::WaterService& TerrainBodyServices::Water() const noexcept
{
    return water_;
}

terrain_gpu::PersistentGpuTerrainCache&
TerrainBodyServices::Cache() noexcept
{
    return cache_;
}

const terrain_gpu::PersistentGpuTerrainCache&
TerrainBodyServices::Cache() const noexcept
{
    return cache_;
}

const std::optional<TerrainBodyServices::BakeRecipe>&
TerrainBodyServices::Recipe() const noexcept
{
    return recipe_;
}

void TerrainBodyServices::SetRecipe(BakeRecipe recipe)
{
    recipe_ = std::move(recipe);
}

const std::shared_ptr<const terrain::BakedTectonicRasters>&
TerrainBodyServices::TectonicBake() const noexcept
{
    return tectonicBake_;
}

void TerrainBodyServices::SetTectonicBake(
    std::shared_ptr<const terrain::BakedTectonicRasters> bake) noexcept
{
    tectonicBake_ = std::move(bake);
}

const std::shared_ptr<const terrain::BakedRiverNetwork>&
TerrainBodyServices::RiverBake() const noexcept
{
    return riverBake_;
}

void TerrainBodyServices::SetRiverBake(
    std::shared_ptr<const terrain::BakedRiverNetwork> bake) noexcept
{
    riverBake_ = std::move(bake);
}

const std::shared_ptr<const terrain::BakedGeologyRasters>&
TerrainBodyServices::GeologicalBake() const noexcept
{
    return geologicalBake_;
}

void TerrainBodyServices::SetGeologicalBake(
    std::shared_ptr<const terrain::BakedGeologyRasters> bake) noexcept
{
    geologicalBake_ = std::move(bake);
}

const std::shared_ptr<const terrain_geology::CompiledStratigraphyProfile>&
TerrainBodyServices::Stratigraphy() const noexcept
{
    return stratigraphy_;
}

void TerrainBodyServices::SetStratigraphyProfile(
    std::optional<terrain_geology::StratigraphyProfile> profile)
{
    stratigraphy_.reset();
    if (profile.has_value())
        stratigraphy_ = std::make_shared<const terrain_geology::CompiledStratigraphyProfile>(
            std::move(*profile), geology_);
}

bool TerrainBodyServices::IsValid() const noexcept
{
    return
        body_.IsValid() &&
        defaultBedrock_.IsValid() &&
        processes_.IsValid() &&
        biomes_.BaseBiome().IsValid() &&
        water_.IsValid() &&
        cache_.Config().IsValid();
}
} // namespace orbit::surface_model
