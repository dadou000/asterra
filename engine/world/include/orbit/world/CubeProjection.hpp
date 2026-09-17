#pragma once

#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <cmath>
#include <optional>

namespace orbit::world
{
// Project a physical unit direction onto a specific cube face rather than
// selecting one canonical major-axis face. This is required at cube edges and
// corners, where the same physical direction legitimately belongs to two or
// three adjacent pages.
[[nodiscard]] inline std::optional<CubeCoordinate>
ProjectDirectionToCubeFace(
    const math::Double3& direction,
    const CubeFace face,
    const f64 edgeTolerance = 1.0e-12) noexcept
{
    if (!std::isfinite(direction.x) ||
        !std::isfinite(direction.y) ||
        !std::isfinite(direction.z) ||
        math::LengthSquared(direction) <= 0.0)
    {
        return std::nullopt;
    }

    const math::Double3 unit =
        math::Normalize(direction);

    f64 denominator = 0.0;
    math::Double2 uv{};

    switch (face)
    {
    case CubeFace::PositiveX:
        denominator = unit.x;
        if (denominator <= 0.0)
        {
            return std::nullopt;
        }
        uv = {
            -unit.z / denominator,
            unit.y / denominator
        };
        break;

    case CubeFace::NegativeX:
        denominator = -unit.x;
        if (denominator <= 0.0)
        {
            return std::nullopt;
        }
        uv = {
            unit.z / denominator,
            unit.y / denominator
        };
        break;

    case CubeFace::PositiveY:
        denominator = unit.y;
        if (denominator <= 0.0)
        {
            return std::nullopt;
        }
        uv = {
            unit.x / denominator,
            -unit.z / denominator
        };
        break;

    case CubeFace::NegativeY:
        denominator = -unit.y;
        if (denominator <= 0.0)
        {
            return std::nullopt;
        }
        uv = {
            unit.x / denominator,
            unit.z / denominator
        };
        break;

    case CubeFace::PositiveZ:
        denominator = unit.z;
        if (denominator <= 0.0)
        {
            return std::nullopt;
        }
        uv = {
            unit.x / denominator,
            unit.y / denominator
        };
        break;

    case CubeFace::NegativeZ:
        denominator = -unit.z;
        if (denominator <= 0.0)
        {
            return std::nullopt;
        }
        uv = {
            -unit.x / denominator,
            unit.y / denominator
        };
        break;
    }

    const f64 tolerance =
        std::max(edgeTolerance, 0.0);

    if (std::abs(uv.x) > 1.0 + tolerance ||
        std::abs(uv.y) > 1.0 + tolerance)
    {
        return std::nullopt;
    }

    uv.x = std::clamp(uv.x, -1.0, 1.0);
    uv.y = std::clamp(uv.y, -1.0, 1.0);

    return CubeCoordinate{
        .face = face,
        .uv = uv
    };
}
} // namespace orbit::world
