#include <orbit/terrain/BakedTectonics.hpp>

#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace orbit::terrain
{
namespace
{
[[nodiscard]] std::size_t GutterCount(const u32 resolution) noexcept
{
    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    return kBakedTectonicFaces * stride * stride;
}

[[nodiscard]] std::size_t GutterIndex(
    const u32 resolution,
    const u32 face,
    const i32 x,
    const i32 y) noexcept
{
    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    return (static_cast<std::size_t>(face) * stride +
            static_cast<std::size_t>(y + 1)) * stride +
           static_cast<std::size_t>(x + 1);
}
} // namespace

math::Double3 BakedTectonicTexelDirection(
    const u32 face,
    const i32 x,
    const i32 y,
    const u32 resolution) noexcept
{
    // Same face mapping as world::CubeToUnitDirection, without its clamp, so
    // gutter texels continue past the face edge onto the neighbouring face's
    // geometry.
    const f64 u =
        (static_cast<f64>(x) + 0.5) / static_cast<f64>(resolution) * 2.0 - 1.0;
    const f64 v =
        (static_cast<f64>(y) + 0.5) / static_cast<f64>(resolution) * 2.0 - 1.0;

    math::Double3 cube{};
    switch (static_cast<world::CubeFace>(face))
    {
    case world::CubeFace::PositiveX: cube = {1.0, v, -u}; break;
    case world::CubeFace::NegativeX: cube = {-1.0, v, u}; break;
    case world::CubeFace::PositiveY: cube = {u, 1.0, -v}; break;
    case world::CubeFace::NegativeY: cube = {u, -1.0, v}; break;
    case world::CubeFace::PositiveZ: cube = {u, v, 1.0}; break;
    case world::CubeFace::NegativeZ: cube = {-u, v, -1.0}; break;
    }
    return math::Normalize(cube);
}

BakedTectonicRasters BakedTectonicRasters::FromFloats(
    const u32 resolution,
    const u64 recipeHash,
    const std::array<std::vector<f32>, kBakedTectonicLayerCount>& layers,
    std::vector<u8> plate,
    std::vector<u8> neighbour,
    const std::array<u8, kBakedTectonicMaxPlates>& plateContinental,
    const u32 plateCount)
{
    if (resolution < 2U || resolution > 4096U)
    {
        throw std::invalid_argument(
            "Baked tectonic resolution must be between 2 and 4096.");
    }

    const std::size_t expected = GutterCount(resolution);
    const std::size_t plateExpected =
        static_cast<std::size_t>(kBakedTectonicFaces) * resolution * resolution;
    if (plate.size() != plateExpected || neighbour.size() != plateExpected)
    {
        throw std::invalid_argument(
            "Baked tectonic plate rasters have the wrong size.");
    }

    BakedTectonicRasters result;
    result.resolution_ = resolution;
    result.recipeHash_ = recipeHash;
    result.plateCount_ = plateCount;
    result.plate_ = std::move(plate);
    result.neighbour_ = std::move(neighbour);
    result.plateContinental_ = plateContinental;

    for (std::size_t layer = 0; layer < kBakedTectonicLayerCount; ++layer)
    {
        const auto& values = layers[layer];
        if (values.size() != expected)
        {
            throw std::invalid_argument(
                "Baked tectonic layer has the wrong size.");
        }

        f32 minimum = std::numeric_limits<f32>::max();
        f32 maximum = std::numeric_limits<f32>::lowest();
        for (const f32 value : values)
        {
            if (!std::isfinite(value))
            {
                throw std::invalid_argument(
                    "Baked tectonic layers must be finite.");
            }
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        if (!(maximum > minimum))
        {
            maximum = minimum + 1.0F;
        }
        result.rangeMinimum_[layer] = minimum;
        result.rangeMaximum_[layer] = maximum;

        auto& quantized = result.layers_[layer];
        quantized.resize(expected);
        const f64 scale = 65535.0 / (static_cast<f64>(maximum) - minimum);
        for (std::size_t i = 0; i < expected; ++i)
        {
            const f64 q = (static_cast<f64>(values[i]) - minimum) * scale;
            quantized[i] = static_cast<u16>(std::clamp(q + 0.5, 0.0, 65535.0));
        }
    }

    result.Finalize();
    return result;
}

BakedTectonicRasters BakedTectonicRasters::FromQuantized(
    const u32 resolution,
    const u64 recipeHash,
    std::array<std::vector<u16>, kBakedTectonicLayerCount> layers,
    std::array<f32, kBakedTectonicLayerCount> rangeMinimum,
    std::array<f32, kBakedTectonicLayerCount> rangeMaximum,
    std::vector<u8> plate,
    std::vector<u8> neighbour,
    const std::array<u8, kBakedTectonicMaxPlates>& plateContinental,
    const u32 plateCount)
{
    if (resolution < 2U || resolution > 4096U)
    {
        throw std::invalid_argument(
            "Baked tectonic resolution must be between 2 and 4096.");
    }

    const std::size_t expected = GutterCount(resolution);
    const std::size_t plateExpected =
        static_cast<std::size_t>(kBakedTectonicFaces) * resolution * resolution;
    if (plate.size() != plateExpected || neighbour.size() != plateExpected)
    {
        throw std::invalid_argument(
            "Baked tectonic plate rasters have the wrong size.");
    }
    for (std::size_t layer = 0; layer < kBakedTectonicLayerCount; ++layer)
    {
        if (layers[layer].size() != expected ||
            !std::isfinite(rangeMinimum[layer]) ||
            !std::isfinite(rangeMaximum[layer]) ||
            !(rangeMaximum[layer] > rangeMinimum[layer]))
        {
            throw std::invalid_argument(
                "Baked tectonic layer data is invalid.");
        }
    }

    BakedTectonicRasters result;
    result.resolution_ = resolution;
    result.recipeHash_ = recipeHash;
    result.plateCount_ = plateCount;
    result.layers_ = std::move(layers);
    result.rangeMinimum_ = rangeMinimum;
    result.rangeMaximum_ = rangeMaximum;
    result.plate_ = std::move(plate);
    result.neighbour_ = std::move(neighbour);
    result.plateContinental_ = plateContinental;
    result.Finalize();
    return result;
}

void BakedTectonicRasters::Finalize()
{
    // Identity of the baked content, independent of how it was produced, so
    // terrain revisions change exactly when the data does.
    u64 hash = StableCombine64(0x4241544543544F4EULL, recipeHash_);
    hash = StableCombine64(hash, resolution_);
    hash = StableCombine64(hash, plateCount_);
    for (std::size_t layer = 0; layer < kBakedTectonicLayerCount; ++layer)
    {
        hash = StableCombine64(hash, std::bit_cast<u32>(rangeMinimum_[layer]));
        hash = StableCombine64(hash, std::bit_cast<u32>(rangeMaximum_[layer]));
        // Every texel: this hash is the terrain cache identity, so it must
        // change whenever any baked value does.
        for (const u16 value : layers_[layer])
        {
            hash = StableCombine64(hash, value);
        }
    }
    for (const u8 value : plate_)
    {
        hash = StableCombine64(hash, value);
    }
    for (const u8 value : neighbour_)
    {
        hash = StableCombine64(hash, value);
    }
    contentHash_ = hash == 0U ? 1U : hash;
}

f32 BakedTectonicRasters::Dequantize(
    const std::size_t layer,
    const std::size_t index) const noexcept
{
    const f32 minimum = rangeMinimum_[layer];
    const f32 maximum = rangeMaximum_[layer];
    return minimum +
           (maximum - minimum) * (static_cast<f32>(layers_[layer][index]) / 65535.0F);
}

BakedTectonicTexel BakedTectonicRasters::Sample(
    const math::Double3& direction) const noexcept
{
    const world::CubeCoordinate cube = world::UnitDirectionToCube(direction);
    const u32 face = static_cast<u32>(cube.face);
    const f64 resolution = static_cast<f64>(resolution_);

    // Texel centres sit at (index + 0.5) / resolution in face space.
    const f64 fx = (cube.uv.x + 1.0) * 0.5 * resolution - 0.5;
    const f64 fy = (cube.uv.y + 1.0) * 0.5 * resolution - 0.5;
    const i32 lastGutter = static_cast<i32>(resolution_);
    const i32 x0 = std::clamp(static_cast<i32>(std::floor(fx)), -1, lastGutter - 1);
    const i32 y0 = std::clamp(static_cast<i32>(std::floor(fy)), -1, lastGutter - 1);
    const f32 tx = static_cast<f32>(std::clamp(fx - static_cast<f64>(x0), 0.0, 1.0));
    const f32 ty = static_cast<f32>(std::clamp(fy - static_cast<f64>(y0), 0.0, 1.0));

    const std::size_t i00 = GutterIndex(resolution_, face, x0, y0);
    const std::size_t i10 = GutterIndex(resolution_, face, x0 + 1, y0);
    const std::size_t i01 = GutterIndex(resolution_, face, x0, y0 + 1);
    const std::size_t i11 = GutterIndex(resolution_, face, x0 + 1, y0 + 1);

    BakedTectonicTexel texel;
    for (std::size_t layer = 0; layer < kBakedTectonicLayerCount; ++layer)
    {
        const f32 a = Dequantize(layer, i00);
        const f32 b = Dequantize(layer, i10);
        const f32 c = Dequantize(layer, i01);
        const f32 d = Dequantize(layer, i11);
        texel.values[layer] =
            (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
    }

    const i32 nx = std::clamp(static_cast<i32>(std::floor(fx + 0.5)), 0, lastGutter - 1);
    const i32 ny = std::clamp(static_cast<i32>(std::floor(fy + 0.5)), 0, lastGutter - 1);
    const std::size_t plateIndex =
        (static_cast<std::size_t>(face) * resolution_ + static_cast<std::size_t>(ny)) *
            resolution_ +
        static_cast<std::size_t>(nx);
    texel.plate = plate_[plateIndex];
    texel.neighbour = neighbour_[plateIndex];
    return texel;
}

BakedTectonicRasters::ConvergenceAndBias
BakedTectonicRasters::SampleConvergenceAndBias(
    const math::Double3& direction) const noexcept
{
    const world::CubeCoordinate cube = world::UnitDirectionToCube(direction);
    const u32 face = static_cast<u32>(cube.face);
    const f64 resolution = static_cast<f64>(resolution_);
    const f64 fx = (cube.uv.x + 1.0) * 0.5 * resolution - 0.5;
    const f64 fy = (cube.uv.y + 1.0) * 0.5 * resolution - 0.5;
    const i32 lastGutter = static_cast<i32>(resolution_);
    const i32 x0 = std::clamp(static_cast<i32>(std::floor(fx)), -1, lastGutter - 1);
    const i32 y0 = std::clamp(static_cast<i32>(std::floor(fy)), -1, lastGutter - 1);
    const f32 tx = static_cast<f32>(std::clamp(fx - static_cast<f64>(x0), 0.0, 1.0));
    const f32 ty = static_cast<f32>(std::clamp(fy - static_cast<f64>(y0), 0.0, 1.0));

    const std::size_t i00 = GutterIndex(resolution_, face, x0, y0);
    const std::size_t i10 = GutterIndex(resolution_, face, x0 + 1, y0);
    const std::size_t i01 = GutterIndex(resolution_, face, x0, y0 + 1);
    const std::size_t i11 = GutterIndex(resolution_, face, x0 + 1, y0 + 1);

    const auto bilinear = [&](const BakedTectonicLayer layer)
    {
        const std::size_t l = static_cast<std::size_t>(layer);
        const f32 a = Dequantize(l, i00);
        const f32 b = Dequantize(l, i10);
        const f32 c = Dequantize(l, i01);
        const f32 d = Dequantize(l, i11);
        return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
    };
    return {
        .convergence = bilinear(BakedTectonicLayer::OrogenEnvelope),
        .plateBiasMeters = bilinear(BakedTectonicLayer::PlateBiasMeters),
        .structuralElevationMeters =
            bilinear(BakedTectonicLayer::StructuralElevationMeters),
        .collisionLand = bilinear(BakedTectonicLayer::CollisionLand)};
}

std::vector<f32> BakedTectonicRasters::BuildGpuConvergenceAndBias() const
{
    const std::size_t count = GutterCount(resolution_);
    std::vector<f32> result(count * 4U);
    const std::size_t convergence = static_cast<std::size_t>(BakedTectonicLayer::OrogenEnvelope);
    const std::size_t bias = static_cast<std::size_t>(BakedTectonicLayer::PlateBiasMeters);
    const std::size_t structural =
        static_cast<std::size_t>(BakedTectonicLayer::StructuralElevationMeters);
    const std::size_t collision = static_cast<std::size_t>(BakedTectonicLayer::CollisionLand);
    for (std::size_t i = 0; i < count; ++i)
    {
        result[i * 4U] = Dequantize(convergence, i);
        result[i * 4U + 1U] = Dequantize(bias, i);
        result[i * 4U + 2U] = Dequantize(structural, i);
        result[i * 4U + 3U] = Dequantize(collision, i);
    }
    return result;
}

std::size_t BakedTectonicRasters::ByteSize() const noexcept
{
    std::size_t bytes = plate_.size() + neighbour_.size();
    for (const auto& layer : layers_)
    {
        bytes += layer.size() * sizeof(u16);
    }
    return bytes;
}
} // namespace orbit::terrain
