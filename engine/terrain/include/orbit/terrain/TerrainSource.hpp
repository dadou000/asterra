#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain/TerrainFields.hpp>

namespace orbit::terrain
{
struct TerrainQuery
{
    math::Double3 unitDirection{};
    f64 footprintMeters{1.0};
};

class TerrainSource
{
public:
    virtual ~TerrainSource() = default;

    TerrainSource(const TerrainSource&) = delete;
    TerrainSource& operator=(const TerrainSource&) = delete;

    // Streaming workers may call Sample concurrently.
    // Implementations must keep concurrent const sampling thread-safe.
    [[nodiscard]] virtual TerrainSample Sample(
        const TerrainQuery& query) const noexcept = 0;

    // Mutable authoritative sources should increment this whenever
    // previously sampled terrain may have changed. Existing sources may keep
    // this coarse revision; GenerationRevisions maps it into the process
    // domain unless a source can report more precise authority revisions.
    [[nodiscard]] virtual u64 Revision() const noexcept
    {
        return 0;
    }

    // Explicit cache-invalidation contract for physical terrain. Sources that
    // know geology/climate/authoring/biome/water ownership should override
    // this and return those domains directly. The default keeps existing
    // TerrainSource implementations correct by treating Revision as the
    // composed process revision.
    [[nodiscard]] virtual TerrainGenerationRevisions
    GenerationRevisions() const noexcept
    {
        TerrainGenerationRevisions revisions{};
        revisions.processes = Revision();
        return revisions;
    }

protected:
    TerrainSource() = default;
};
} // namespace orbit::terrain
