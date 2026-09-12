#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/jobs/JobSystem.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/terrain_cache/TerrainPage.hpp>
#include <orbit/world/Planet.hpp>

#include <cstddef>
#include <memory>

namespace orbit::terrain_cache
{
class TerrainPageCache
{
public:
    TerrainPageCache(
        world::PlanetDefinition planet,
        std::shared_ptr<const terrain::TerrainSource> source,
        jobs::JobSystem& jobs);

    ~TerrainPageCache();

    TerrainPageCache(const TerrainPageCache&) = delete;
    TerrainPageCache& operator=(const TerrainPageCache&) = delete;
    TerrainPageCache(TerrainPageCache&&) = delete;
    TerrainPageCache& operator=(TerrainPageCache&&) = delete;

    [[nodiscard]] bool Request(const TerrainPageDesc& desc);

    [[nodiscard]] std::shared_ptr<const TerrainPage> TryGet(
        const TerrainPageDesc& desc) const;

    [[nodiscard]] bool IsPending(
        const TerrainPageDesc& desc) const;

    [[nodiscard]] std::size_t EntryCount() const;

    void WaitAll();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::terrain_cache
