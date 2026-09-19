#include <orbit/terrain_debug/TerrainDebugRaster.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace orbit::terrain_debug
{
namespace
{
[[nodiscard]] u8 ByteFromUnit(const f32 value) noexcept
{
    const f32 clamped = std::clamp(value, 0.0F, 1.0F);
    return static_cast<u8>(std::lround(clamped * 255.0F));
}

void PushRgba(std::vector<u8>& output, const u8 r, const u8 g, const u8 b, const u8 a = 255U)
{
    output.push_back(r);
    output.push_back(g);
    output.push_back(b);
    output.push_back(a);
}

void PushInvalid(std::vector<u8>& output)
{
    PushRgba(output, 255U, 0U, 255U);
}

[[nodiscard]] f32 Normalize(const f32 value, const f32 minimum, const f32 maximum) noexcept
{
    if (!(maximum > minimum))
    {
        return 0.5F;
    }
    return std::clamp((value - minimum) / (maximum - minimum), 0.0F, 1.0F);
}

struct NumericRange
{
    f32 minimum{0.0F};
    f32 maximum{1.0F};
};

[[nodiscard]] NumericRange ManualRange(const TerrainDebugRasterRange& requested)
{
    if (!std::isfinite(requested.minimum) || !std::isfinite(requested.maximum) ||
        !(requested.maximum > requested.minimum))
    {
        throw std::invalid_argument(
            "Terrain debug manual range must be finite with maximum > minimum.");
    }
    return {.minimum = requested.minimum, .maximum = requested.maximum};
}

[[nodiscard]] NumericRange AutoScalarRange(const std::span<const f32> values, const bool symmetric)
{
    f32 minimum = std::numeric_limits<f32>::infinity();
    f32 maximum = -std::numeric_limits<f32>::infinity();

    for (const f32 value : values)
    {
        if (!std::isfinite(value))
        {
            continue;
        }
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }

    if (!std::isfinite(minimum) || !std::isfinite(maximum))
    {
        return {};
    }

    if (symmetric)
    {
        const f32 extent = std::max(std::abs(minimum), std::abs(maximum));
        if (extent <= 0.0F)
        {
            return {.minimum = -1.0F, .maximum = 1.0F};
        }
        return {.minimum = -extent, .maximum = extent};
    }

    if (!(maximum > minimum))
    {
        const f32 pad = std::max(std::abs(minimum) * 0.05F, 1.0F);
        return {.minimum = minimum - pad, .maximum = maximum + pad};
    }

    return {.minimum = minimum, .maximum = maximum};
}

[[nodiscard]] NumericRange AutoVectorRange(
    const std::span<const TerrainDebugVector2> values)
{
    f32 maximum = 0.0F;
    for (const auto value : values)
    {
        if (!std::isfinite(value.x) || !std::isfinite(value.y))
        {
            continue;
        }
        maximum = std::max(
            maximum,
            std::sqrt(value.x * value.x + value.y * value.y));
    }
    return {.minimum = 0.0F, .maximum = maximum > 0.0F ? maximum : 1.0F};
}

[[nodiscard]] u64 HashCategory(u64 value, const u64 seed) noexcept
{
    value = terrain::StableCombine64(seed, value);
    return terrain::StableMix64(value);
}

void PushCategory(std::vector<u8>& output, const u64 value, const u64 seed)
{
    const u64 hash = HashCategory(value, seed);
    const auto channel = [hash](const u32 shift)
    {
        return static_cast<u8>(64U + ((hash >> shift) & 0xBFU));
    };
    PushRgba(output, channel(0U), channel(8U), channel(16U));
}

void RequireSize(const u64 expected, const std::size_t actual, const char* name)
{
    if (expected != static_cast<u64>(actual))
    {
        throw std::invalid_argument(
            std::string("Terrain debug ") + name +
            " source size does not match raster dimensions.");
    }
}
} // namespace

u64 TerrainDebugTexelCount(const TerrainDebugRasterView& view) noexcept
{
    return static_cast<u64>(view.width) * static_cast<u64>(view.height);
}

std::vector<u8> ComposeTerrainDebugRgba8(const TerrainDebugRasterView& view)
{
    const u64 texels = TerrainDebugTexelCount(view);
    if (view.width == 0U || view.height == 0U ||
        texels > static_cast<u64>(std::numeric_limits<std::size_t>::max() / 4U))
    {
        throw std::invalid_argument("Terrain debug raster dimensions are invalid.");
    }

    const auto& descriptor = Descriptor(view.field);
    std::vector<u8> output;
    output.reserve(static_cast<std::size_t>(texels) * 4U);

    switch (descriptor.valueClass)
    {
    case TerrainDebugValueClass::Scalar:
    case TerrainDebugValueClass::SignedScalar:
    {
        RequireSize(texels, view.scalar.size(), "scalar");
        const bool signedField =
            descriptor.valueClass == TerrainDebugValueClass::SignedScalar;
        const NumericRange range = view.range.automatic
            ? AutoScalarRange(view.scalar, signedField)
            : ManualRange(view.range);

        for (const f32 value : view.scalar)
        {
            if (!std::isfinite(value))
            {
                PushInvalid(output);
                continue;
            }

            const f32 t = Normalize(value, range.minimum, range.maximum);
            if (!signedField)
            {
                const u8 gray = ByteFromUnit(t);
                PushRgba(output, gray, gray, gray);
                continue;
            }

            if (t < 0.5F)
            {
                const f32 x = t * 2.0F;
                PushRgba(output, ByteFromUnit(x), ByteFromUnit(x), 255U);
            }
            else
            {
                const f32 x = (1.0F - t) * 2.0F;
                PushRgba(output, 255U, ByteFromUnit(x), ByteFromUnit(x));
            }
        }
        break;
    }

    case TerrainDebugValueClass::Vector:
    {
        RequireSize(texels, view.vector.size(), "vector");
        const NumericRange range = view.range.automatic
            ? AutoVectorRange(view.vector)
            : ManualRange(view.range);

        for (const auto value : view.vector)
        {
            if (!std::isfinite(value.x) || !std::isfinite(value.y))
            {
                PushInvalid(output);
                continue;
            }

            const f32 magnitude =
                std::sqrt(value.x * value.x + value.y * value.y);
            const f32 inverse = magnitude > 1.0e-12F ? 1.0F / magnitude : 0.0F;

            PushRgba(
                output,
                ByteFromUnit(value.x * inverse * 0.5F + 0.5F),
                ByteFromUnit(value.y * inverse * 0.5F + 0.5F),
                ByteFromUnit(Normalize(magnitude, range.minimum, range.maximum)));
        }
        break;
    }

    case TerrainDebugValueClass::Category:
        RequireSize(texels, view.category.size(), "category");
        for (const u32 value : view.category)
        {
            PushCategory(output, value, 0x4D32394341544547ULL);
        }
        break;

    case TerrainDebugValueClass::Boolean:
        RequireSize(texels, view.boolean.size(), "boolean");
        for (const u8 value : view.boolean)
        {
            if (value == 0U)
            {
                PushRgba(output, 24U, 24U, 24U);
            }
            else
            {
                PushRgba(output, 64U, 224U, 112U);
            }
        }
        break;

    case TerrainDebugValueClass::Revision:
        RequireSize(texels, view.revision.size(), "revision");
        for (const u64 value : view.revision)
        {
            PushCategory(output, value, 0x4D32395245564953ULL);
        }
        break;

    case TerrainDebugValueClass::Lod:
        RequireSize(texels, view.lod.size(), "LOD");
        for (const u8 value : view.lod)
        {
            PushCategory(output, value, 0x4D32394C4F444C56ULL);
        }
        break;
    }

    return output;
}
} // namespace orbit::terrain_debug
