#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

namespace orbit::terrain
{
struct TerrainQuery
{
    math::Double3 unitDirection{};
    f64 footprintMeters{1.0};
};

struct TerrainSample
{
    f64 elevationMeters{0.0};
};

class TerrainSource
{
public:
    virtual ~TerrainSource() = default;

    TerrainSource(const TerrainSource&) = delete;
    TerrainSource& operator=(const TerrainSource&) = delete;

    [[nodiscard]] virtual TerrainSample Sample(
        const TerrainQuery& query) const noexcept = 0;

protected:
    TerrainSource() = default;
};
} // namespace orbit::terrain
