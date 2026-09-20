#include <orbit/surface_model/TerrainBodyServices.hpp>

#include <stdexcept>

namespace orbit::surface_model
{
bool TerrainProcessService::IsValid() const noexcept
{
    return
        streamPower.IsValid() &&
        hydraulic.IsValid() &&
        thermal.IsValid() &&
        aeolian.IsValid() &&
        glacial.IsValid() &&
        rivers.IsValid() &&
        coastal.IsValid();
}

TerrainBodyServices::TerrainBodyServices(
    const universe::BodyId body,
    const terrain_gpu::PersistentGpuTerrainCacheConfig cacheConfig)
    : body_(body),
      biomes_(body),
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

bool TerrainBodyServices::IsValid() const noexcept
{
    return
        body_.IsValid() &&
        defaultBedrock_.IsValid() &&
        processes_.IsValid() &&
        biomes_.BaseBiome().IsValid() &&
        cache_.Config().IsValid();
}
} // namespace orbit::surface_model
