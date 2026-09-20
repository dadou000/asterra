#pragma once

#include <orbit/scene/ObjectStore.hpp>
#include <orbit/surface/SurfaceRegistry.hpp>
#include <orbit/surface_model/TerrainBodyServices.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/world_model/UniverseComposition.hpp>

#include <memory>
#include <optional>
#include <unordered_map>

namespace orbit::surface_model
{
struct SurfaceCompositionStats
{
    u32 terrainSurfaces{0};
    u32 biomeServices{0};
    u32 biomeDefinitions{0};
    u64 sourceRevision{0};
};

// Reconstructs concrete body surface capabilities from authoritative semantic
// children. The registry is rebuilt whenever UniverseComposition changes
// because SurfaceRegistry intentionally references the active BodyRegistry.
class SurfaceComposition
{
public:
    SurfaceComposition() = default;
    ~SurfaceComposition() = default;

    SurfaceComposition(const SurfaceComposition&) = delete;
    SurfaceComposition& operator=(const SurfaceComposition&) = delete;
    SurfaceComposition(SurfaceComposition&&) noexcept = default;
    SurfaceComposition& operator=(SurfaceComposition&&) noexcept = default;

    [[nodiscard]] SurfaceCompositionStats Rebuild(
        const scene::ObjectStore& objects,
        const world_model::UniverseComposition& universe);

    [[nodiscard]] surface::SurfaceRegistry& Registry();
    [[nodiscard]] const surface::SurfaceRegistry& Registry() const;

    [[nodiscard]] std::optional<universe::BodyId>
    BodyForTerrainObject(scene::ObjectId object) const noexcept;

    [[nodiscard]] std::optional<scene::ObjectId>
    TerrainObjectForBody(universe::BodyId body) const noexcept;

    [[nodiscard]] TerrainBodyServices* ServicesForBody(
        universe::BodyId body) noexcept;
    [[nodiscard]] const TerrainBodyServices* ServicesForBody(
        universe::BodyId body) const noexcept;

    [[nodiscard]] terrain_geology::GeologicalMaterialLibrary* GeologyForBody(
        universe::BodyId body) noexcept;
    [[nodiscard]] const terrain_geology::GeologicalMaterialLibrary* GeologyForBody(
        universe::BodyId body) const noexcept;

    [[nodiscard]] TerrainProcessService* ProcessesForBody(
        universe::BodyId body) noexcept;
    [[nodiscard]] const TerrainProcessService* ProcessesForBody(
        universe::BodyId body) const noexcept;

    [[nodiscard]] terrain_biome::BiomeService* BiomesForBody(
        universe::BodyId body) noexcept;
    [[nodiscard]] const terrain_biome::BiomeService* BiomesForBody(
        universe::BodyId body) const noexcept;

    [[nodiscard]] terrain_gpu::PersistentGpuTerrainCache* CacheForBody(
        universe::BodyId body) noexcept;
    [[nodiscard]] const terrain_gpu::PersistentGpuTerrainCache* CacheForBody(
        universe::BodyId body) const noexcept;

    [[nodiscard]] u64 SourceRevision() const noexcept;

private:
    std::unique_ptr<surface::SurfaceRegistry> registry_;
    std::unordered_map<scene::ObjectId, universe::BodyId>
        bodyByTerrainObject_;
    std::unordered_map<universe::BodyId, scene::ObjectId>
        terrainObjectByBody_;
    std::unordered_map<
        universe::BodyId,
        std::unique_ptr<TerrainBodyServices>>
        servicesByBody_;
    u64 sourceRevision_{~u64{0}};
};
} // namespace orbit::surface_model
