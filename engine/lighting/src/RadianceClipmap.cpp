#include <orbit/lighting/RadianceClipmap.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] f32 FiniteNonNegative(
    const f32 value) noexcept
{
    return
        std::isfinite(value)
            ? std::max(value, 0.0F)
            : 0.0F;
}

[[nodiscard]] f32 SaturatingRevision24(
    const u64 revision) noexcept
{
    constexpr u64 kMask = 0x00FF'FFFFULL;
    return static_cast<f32>(
        revision & kMask);
}

[[nodiscard]] i64 Quantize(
    const f64 coordinate,
    const f64 cellSize)
{
    if (!std::isfinite(coordinate))
    {
        throw std::invalid_argument(
            "Radiance clipmap coordinate must be finite.");
    }

    return static_cast<i64>(
        std::floor(
            coordinate / cellSize));
}
} // namespace

math::Float3 EvaluateIrradiance(
    const DirectionalIrradianceL1& irradiance,
    math::Float3 unitDirection) noexcept
{
    const f32 lengthSquared =
        math::LengthSquared(unitDirection);

    if (!std::isfinite(lengthSquared) ||
        lengthSquared <= 1.0e-12F)
    {
        unitDirection =
            {0.0F, 1.0F, 0.0F};
    }
    else
    {
        unitDirection =
            math::Normalize(unitDirection);
    }

    const math::Float3 value{
        irradiance.l0.x +
            irradiance.l1x.x * unitDirection.x +
            irradiance.l1y.x * unitDirection.y +
            irradiance.l1z.x * unitDirection.z,
        irradiance.l0.y +
            irradiance.l1x.y * unitDirection.x +
            irradiance.l1y.y * unitDirection.y +
            irradiance.l1z.y * unitDirection.z,
        irradiance.l0.z +
            irradiance.l1x.z * unitDirection.x +
            irradiance.l1y.z * unitDirection.y +
            irradiance.l1z.z * unitDirection.z
    };

    return {
        std::max(value.x, 0.0F),
        std::max(value.y, 0.0F),
        std::max(value.z, 0.0F)
    };
}

GpuRadianceCell EncodeGpuRadianceCell(
    const RadianceCell& cell) noexcept
{
    return {
        .irradiance0 = {
            FiniteNonNegative(
                cell.irradiance.l0.x),
            FiniteNonNegative(
                cell.irradiance.l0.y),
            FiniteNonNegative(
                cell.irradiance.l0.z),
            cell.valid ? 1.0F : 0.0F
        },
        .irradianceX = {
            std::isfinite(cell.irradiance.l1x.x)
                ? cell.irradiance.l1x.x
                : 0.0F,
            std::isfinite(cell.irradiance.l1x.y)
                ? cell.irradiance.l1x.y
                : 0.0F,
            std::isfinite(cell.irradiance.l1x.z)
                ? cell.irradiance.l1x.z
                : 0.0F,
            FiniteNonNegative(
                cell.updateAgeSeconds)
        },
        .irradianceY = {
            std::isfinite(cell.irradiance.l1y.x)
                ? cell.irradiance.l1y.x
                : 0.0F,
            std::isfinite(cell.irradiance.l1y.y)
                ? cell.irradiance.l1y.y
                : 0.0F,
            std::isfinite(cell.irradiance.l1y.z)
                ? cell.irradiance.l1y.z
                : 0.0F,
            static_cast<f32>(
                cell.sampleCount)
        },
        .irradianceZ = {
            std::isfinite(cell.irradiance.l1z.x)
                ? cell.irradiance.l1z.x
                : 0.0F,
            std::isfinite(cell.irradiance.l1z.y)
                ? cell.irradiance.l1z.y
                : 0.0F,
            std::isfinite(cell.irradiance.l1z.z)
                ? cell.irradiance.l1z.z
                : 0.0F,
            SaturatingRevision24(
                cell.revision)
        }
    };
}

f64 RadianceCellSizeMeters(
    const RadianceClipmapConfig& config,
    const u32 level)
{
    if (!std::isfinite(
            config.baseCellSizeMeters) ||
        config.baseCellSizeMeters <= 0.0 ||
        !std::isfinite(config.levelScale) ||
        config.levelScale <= 1.0 ||
        config.levelCount == 0U ||
        level >= config.levelCount)
    {
        throw std::invalid_argument(
            "Radiance clipmap configuration or level is invalid.");
    }

    const f64 size =
        config.baseCellSizeMeters *
        std::pow(
            config.levelScale,
            static_cast<f64>(level));

    if (!std::isfinite(size) ||
        size <= 0.0)
    {
        throw std::overflow_error(
            "Radiance clipmap cell size overflowed.");
    }

    return size;
}

RadianceCellKey RadianceCellForPoint(
    const math::Double3& pointInFrameMeters,
    const RadianceClipmapConfig& config,
    const u32 level,
    const LightingView& view)
{
    if (!view.frame || !view.body)
    {
        throw std::invalid_argument(
            "Radiance clipmap addressing requires a valid frame and body.");
    }

    const f64 cellSize =
        RadianceCellSizeMeters(
            config,
            level);

    return {
        .frame = view.frame,
        .body = view.body,
        .level = level,
        .x = Quantize(
            pointInFrameMeters.x,
            cellSize),
        .y = Quantize(
            pointInFrameMeters.y,
            cellSize),
        .z = Quantize(
            pointInFrameMeters.z,
            cellSize)
    };
}

math::Double3 RadianceCellCenterInFrame(
    const RadianceCellKey& key,
    const RadianceClipmapConfig& config)
{
    if (!key.frame || !key.body)
    {
        throw std::invalid_argument(
            "Radiance cell key requires valid frame/body authority.");
    }

    const f64 cellSize =
        RadianceCellSizeMeters(
            config,
            key.level);

    return {
        (static_cast<f64>(key.x) + 0.5) *
            cellSize,
        (static_cast<f64>(key.y) + 0.5) *
            cellSize,
        (static_cast<f64>(key.z) + 0.5) *
            cellSize
    };
}

math::Float3 RadianceCellGpuCenter(
    const RadianceCellKey& key,
    const RadianceClipmapConfig& config,
    const LightingView& view)
{
    if (key.frame != view.frame ||
        key.body != view.body)
    {
        throw std::invalid_argument(
            "Radiance cell key does not belong to the active LightingView.");
    }

    return ToLightingCameraRelative(
        RadianceCellCenterInFrame(
            key,
            config),
        view);
}

RadianceClipmapMemoryLayout
BuildRadianceClipmapMemoryLayout(
    const RadianceClipmapConfig& config)
{
    if (config.levelCount == 0U ||
        config.cellsPerAxis == 0U)
    {
        throw std::invalid_argument(
            "Radiance clipmap requires non-zero levels and resolution.");
    }

    RadianceClipmapMemoryLayout result;
    result.levels.reserve(
        config.levelCount);

    const u64 axis =
        static_cast<u64>(
            config.cellsPerAxis);

    if (axis >
        std::numeric_limits<u64>::max() /
            axis)
    {
        throw std::overflow_error(
            "Radiance clipmap resolution overflows cell count.");
    }

    const u64 square =
        axis * axis;

    if (square >
        std::numeric_limits<u64>::max() /
            axis)
    {
        throw std::overflow_error(
            "Radiance clipmap resolution overflows cell count.");
    }

    const u64 cellCount =
        square * axis;

    for (u32 level = 0U;
         level < config.levelCount;
         ++level)
    {
        const f64 cellSize =
            RadianceCellSizeMeters(
                config,
                level);

        if (cellCount >
            std::numeric_limits<u64>::max() /
                result.bytesPerCell)
        {
            throw std::overflow_error(
                "Radiance clipmap level byte size overflows.");
        }

        const u64 byteSize =
            cellCount *
            result.bytesPerCell;

        if (result.totalCells >
                std::numeric_limits<u64>::max() -
                    cellCount ||
            result.totalBytes >
                std::numeric_limits<u64>::max() -
                    byteSize)
        {
            throw std::overflow_error(
                "Radiance clipmap total memory size overflows.");
        }

        result.levels.push_back({
            .level = level,
            .cellSizeMeters =
                cellSize,
            .cellsPerAxis =
                config.cellsPerAxis,
            .cellCount =
                cellCount,
            .byteSize =
                byteSize
        });

        result.totalCells +=
            cellCount;
        result.totalBytes +=
            byteSize;
    }

    return result;
}
} // namespace orbit::lighting
