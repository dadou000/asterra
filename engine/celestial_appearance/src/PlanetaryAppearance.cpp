#include <orbit/celestial_appearance/PlanetaryAppearance.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <execution>
#include <numeric>
#include <vector>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

namespace orbit::celestial_appearance
{
namespace
{
[[nodiscard]] math::Double3 FaceDirection(
    const u32 face,
    const f64 u,
    const f64 v)
{
    math::Double3 p{};

    switch (face)
    {
    case 0: p = { 1.0, v, -u}; break;
    case 1: p = {-1.0, v,  u}; break;
    case 2: p = { u, 1.0, -v}; break;
    case 3: p = { u,-1.0,  v}; break;
    case 4: p = { u, v, 1.0}; break;
    default:p = {-u, v,-1.0}; break;
    }

    return math::Normalize(p);
}

[[nodiscard]] f32 Saturate(
    const f64 value) noexcept
{
    return static_cast<f32>(
        std::clamp(value, 0.0, 1.0));
}

[[nodiscard]] math::Float3 Mix(
    const math::Float3 a,
    const math::Float3 b,
    const f32 t) noexcept
{
    return a * (1.0F - t) + b * t;
}

[[nodiscard]] math::Float3 BiomeAlbedo(
    const terrain::BiomeWeights& b) noexcept
{
    const math::Float3 ocean{0.015F, 0.055F, 0.095F};
    const math::Float3 desert{0.48F, 0.36F, 0.20F};
    const math::Float3 grass{0.13F, 0.28F, 0.10F};
    const math::Float3 temperate{0.055F, 0.18F, 0.065F};
    const math::Float3 boreal{0.045F, 0.12F, 0.065F};
    const math::Float3 tundra{0.34F, 0.36F, 0.32F};
    const math::Float3 alpine{0.31F, 0.30F, 0.28F};
    const math::Float3 wetland{0.065F, 0.16F, 0.10F};

    return
        ocean * b.ocean +
        desert * b.desert +
        grass * b.grassland +
        temperate * b.temperateForest +
        boreal * b.borealForest +
        tundra * b.tundra +
        alpine * b.alpine +
        wetland * b.wetland;
}

[[nodiscard]] f32 BiomeRoughness(
    const terrain::BiomeWeights& b) noexcept
{
    return std::clamp(
        0.58F * b.ocean +
        0.88F * b.desert +
        0.82F * b.grassland +
        0.86F * b.temperateForest +
        0.90F * b.borealForest +
        0.91F * b.tundra +
        0.94F * b.alpine +
        0.76F * b.wetland,
        0.04F,
        1.0F);
}

[[nodiscard]] u64 BuildFingerprint(
    const terrain::TerrainSource& source,
    const f64 referenceRadiusMeters,
    const AppearanceConfig& config)
{
    u64 value =
        0x4D31364150504541ULL;

    const auto revisions =
        source.GenerationRevisions();

    value = terrain::StableCombine64(
        value,
        terrain::RevisionFingerprint(
            revisions));
    value = terrain::StableCombine64(
        value,
        source.Revision());
    value = terrain::StableCombine64(
        value,
        std::bit_cast<u64>(
            referenceRadiusMeters));
    value = terrain::StableCombine64(
        value,
        config.faceResolution);
    value = terrain::StableCombine64(
        value,
        std::bit_cast<u64>(
            config.footprintScale));
    value = terrain::StableCombine64(
        value,
        std::bit_cast<u64>(
            config.iceFreezeTemperatureC));
    value = terrain::StableCombine64(
        value,
        std::bit_cast<u64>(
            config.iceFullTemperatureC));

    return value;
}
} // namespace

const AppearanceTexel&
PlanetaryAppearanceProduct::At(
    const u32 face,
    const u32 x,
    const u32 y) const
{
    if (face >= 6U ||
        x >= faceResolution ||
        y >= faceResolution)
    {
        throw std::out_of_range(
            "Planetary appearance texel is out of range.");
    }

    const std::size_t faceStride =
        static_cast<std::size_t>(
            faceResolution) *
        faceResolution;

    return texels[
        static_cast<std::size_t>(face) *
            faceStride +
        static_cast<std::size_t>(y) *
            faceResolution +
        x];
}

u64 PlanetaryAppearanceFingerprint(
    const terrain::TerrainSource& source,
    const f64 referenceRadiusMeters,
    const AppearanceConfig& config)
{
    if (!std::isfinite(referenceRadiusMeters) ||
        referenceRadiusMeters <= 0.0 ||
        config.faceResolution < 3U ||
        !std::isfinite(config.footprintScale) ||
        config.footprintScale <= 0.0 ||
        !std::isfinite(config.iceFreezeTemperatureC) ||
        !std::isfinite(config.iceFullTemperatureC) ||
        config.iceFullTemperatureC >=
            config.iceFreezeTemperatureC)
    {
        throw std::invalid_argument(
            "Planetary appearance config is invalid.");
    }

    return BuildFingerprint(
        source,
        referenceRadiusMeters,
        config);
}

PlanetaryAppearanceProduct
BuildPlanetaryAppearance(
    const terrain::TerrainSource& source,
    const f64 referenceRadiusMeters,
    const AppearanceConfig& config)
{
    const u64 fingerprint =
        PlanetaryAppearanceFingerprint(
            source,
            referenceRadiusMeters,
            config);

    PlanetaryAppearanceProduct result;
    result.faceResolution =
        config.faceResolution;
    result.sourceRevisions =
        source.GenerationRevisions();
    result.fingerprint =
        fingerprint;

    const f64 angularCell =
        (0.5 * std::numbers::pi_v<f64>) /
        static_cast<f64>(
            config.faceResolution - 1U);

    result.sampleFootprintMeters =
        referenceRadiusMeters *
        angularCell *
        config.footprintScale;

    const std::size_t faceStride =
        static_cast<std::size_t>(
            config.faceResolution) *
        config.faceResolution;

    result.texels.resize(
        faceStride * 6U);

    const f64 normalStep =
        std::max(
            angularCell * 0.35,
            1.0e-6);

    const auto displaced =
        [&](const math::Double3 direction)
        {
            const auto unit =
                math::Normalize(direction);
            const auto sample =
                source.Sample({
                    .unitDirection = unit,
                    .footprintMeters =
                        result.sampleFootprintMeters
                });

            return unit *
                (referenceRadiusMeters +
                 sample.elevationMeters);
        };

    // Every texel is an independent pure query of the immutable terrain
    // authority, so rows are evaluated in parallel.
    std::vector<u32> rows(
        6U * config.faceResolution);
    std::iota(
        rows.begin(),
        rows.end(),
        0U);

    std::for_each(
        std::execution::par,
        rows.begin(),
        rows.end(),
        [&](const u32 row)
        {
            const u32 face =
                row / config.faceResolution;
            const u32 y =
                row % config.faceResolution;

            const f64 v =
                -1.0 +
                2.0 *
                static_cast<f64>(y) /
                static_cast<f64>(
                    config.faceResolution - 1U);

            for (u32 x = 0;
                 x < config.faceResolution;
                 ++x)
            {
                const f64 u =
                    -1.0 +
                    2.0 *
                    static_cast<f64>(x) /
                    static_cast<f64>(
                        config.faceResolution - 1U);

                const auto direction =
                    FaceDirection(
                        face,
                        u,
                        v);

                const auto sample =
                    source.Sample({
                        .unitDirection =
                            direction,
                        .footprintMeters =
                            result.sampleFootprintMeters
                    });

                const f32 ocean =
                    sample.standingWaterDepthMeters >
                            0.01
                        ? 1.0F
                        : 0.0F;

                const f64 iceT =
                    (static_cast<f64>(
                         config.iceFreezeTemperatureC) -
                     static_cast<f64>(
                         sample.climate.temperatureC)) /
                    std::max(
                        static_cast<f64>(
                            config.iceFreezeTemperatureC -
                            config.iceFullTemperatureC),
                        1.0e-6);

                f32 ice =
                    Saturate(iceT);

                if (ocean < 0.5F)
                {
                    const f32 snowClimate =
                        Saturate(
                            (-2.0 -
                             sample.climate.temperatureC) /
                            18.0);
                    ice = std::max(
                        ice,
                        snowClimate *
                        static_cast<f32>(
                            std::clamp(
                                sample.biomes.tundra +
                                sample.biomes.alpine,
                                0.0F,
                                1.0F)));
                }

                auto albedo =
                    BiomeAlbedo(
                        sample.biomes);

                if (ocean > 0.5F)
                {
                    albedo = {
                        0.012F,
                        0.042F,
                        0.075F
                    };
                }

                albedo =
                    Mix(
                        albedo,
                        {
                            0.76F,
                            0.82F,
                            0.86F
                        },
                        ice);

                f32 roughness =
                    BiomeRoughness(
                        sample.biomes);

                if (ocean > 0.5F)
                {
                    roughness = 0.18F;
                }

                roughness =
                    roughness *
                        (1.0F - ice) +
                    0.34F * ice;

                const math::Double3 reference =
                    std::abs(direction.y) < 0.9
                        ? math::Double3{
                              0.0, 1.0, 0.0}
                        : math::Double3{
                              1.0, 0.0, 0.0};

                const auto tangentA =
                    math::Normalize(
                        math::Cross(
                            reference,
                            direction));
                const auto tangentB =
                    math::Normalize(
                        math::Cross(
                            direction,
                            tangentA));

                const auto aMinus =
                    displaced(
                        direction -
                        tangentA * normalStep);
                const auto aPlus =
                    displaced(
                        direction +
                        tangentA * normalStep);
                const auto bMinus =
                    displaced(
                        direction -
                        tangentB * normalStep);
                const auto bPlus =
                    displaced(
                        direction +
                        tangentB * normalStep);

                auto normal =
                    math::Cross(
                        aPlus - aMinus,
                        bPlus - bMinus);

                if (math::Dot(
                        normal,
                        direction) < 0.0)
                {
                    normal =
                        normal * -1.0;
                }

                normal =
                    math::LengthSquared(normal) >
                            1.0e-24
                        ? math::Normalize(normal)
                        : direction;

                const std::size_t index =
                    static_cast<std::size_t>(
                        face) *
                        faceStride +
                    static_cast<std::size_t>(
                        y) *
                        config.faceResolution +
                    x;

                result.texels[index] = {
                    .albedoLinear = albedo,
                    .normal = {
                        static_cast<f32>(
                            normal.x),
                        static_cast<f32>(
                            normal.y),
                        static_cast<f32>(
                            normal.z)
                    },
                    .roughness = roughness,
                    .oceanMask = ocean,
                    .waterDepthMeters =
                        static_cast<f32>(
                            std::max(
                                sample.standingWaterDepthMeters,
                                0.0)),
                    .iceMask = ice,
                    .directLightTransmittance = 1.0F,
                    // Terrain/climate authority currently provides no
                    // canonical emissive field. Keep the channel explicit
                    // and zero rather than inventing city/lava authority.
                    .emissionLinear = {}
                };
            }
                });

    return result;
}

GpuPlanetaryAppearanceProduct::
GpuPlanetaryAppearanceProduct(
    rhi::Device& device,
    const PlanetaryAppearanceProduct& product)
    : texelCount_(
          static_cast<u32>(
              product.texels.size())),
      faceResolution_(
          product.faceResolution),
      fingerprint_(
          product.fingerprint)
{
    if (product.faceResolution < 3U ||
        product.texels.empty())
    {
        throw std::invalid_argument(
            "GPU planetary appearance product is invalid.");
    }

    std::vector<GpuAppearanceTexel> packed;
    packed.reserve(
        product.texels.size());

    for (const auto& texel :
         product.texels)
    {
        packed.push_back({
            .albedoLinear =
                texel.albedoLinear,
            .roughness =
                texel.roughness,
            .normal =
                texel.normal,
            .oceanMask =
                texel.oceanMask,
            .waterDepthMeters =
                texel.waterDepthMeters,
            .emissionLinear =
                texel.emissionLinear,
            .iceMask =
                texel.iceMask,
            .directLightTransmittance =
                texel.directLightTransmittance
        });
    }

    buffer_ =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    packed.size() *
                    sizeof(GpuAppearanceTexel)),
            .usage =
                rhi::BufferUsage::Structured,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::
                    ShaderResource
        });

    if (!buffer_)
    {
        throw std::runtime_error(
            "Failed to allocate planetary appearance GPU buffer.");
    }

    std::memcpy(
        buffer_->Map(),
        packed.data(),
        packed.size() *
            sizeof(GpuAppearanceTexel));
    buffer_->Unmap();
}

rhi::Buffer&
GpuPlanetaryAppearanceProduct::Buffer() noexcept
{
    return *buffer_;
}

u32 GpuPlanetaryAppearanceProduct::
TexelCount() const noexcept
{
    return texelCount_;
}

u32 GpuPlanetaryAppearanceProduct::
FaceResolution() const noexcept
{
    return faceResolution_;
}

u64 GpuPlanetaryAppearanceProduct::
Fingerprint() const noexcept
{
    return fingerprint_;
}
} // namespace orbit::celestial_appearance
