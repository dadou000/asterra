#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
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
    // previously sampled terrain may have changed.
    [[nodiscard]] virtual u64 Revision() const noexcept
    {
        return 0;
    }

protected:
    TerrainSource() = default;
};
} // namespace orbit::terrain
