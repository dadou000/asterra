#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/terrain/TerrainSource.hpp>

#include <memory>
#include <vector>

namespace orbit::celestial_appearance
{
struct AppearanceConfig
{
    u32 faceResolution{33};
    f64 footprintScale{1.5};
    f64 iceFreezeTemperatureC{-1.5};
    f64 iceFullTemperatureC{-15.0};
};

struct AppearanceTexel
{
    math::Float3 albedoLinear{0.18F, 0.18F, 0.18F};
    math::Float3 normal{0.0F, 1.0F, 0.0F};
    f32 roughness{0.8F};
    f32 oceanMask{0.0F};
    f32 iceMask{0.0F};
    math::Float3 emissionLinear{};
};

struct PlanetaryAppearanceProduct
{
    u32 faceResolution{0};
    f64 sampleFootprintMeters{1.0};
    terrain::TerrainGenerationRevisions sourceRevisions{};
    u64 fingerprint{0};
    std::vector<AppearanceTexel> texels;

    [[nodiscard]] const AppearanceTexel& At(
        u32 face,
        u32 x,
        u32 y) const;
};

[[nodiscard]] u64 PlanetaryAppearanceFingerprint(
    const terrain::TerrainSource& source,
    f64 referenceRadiusMeters,
    const AppearanceConfig& config = {});

[[nodiscard]] PlanetaryAppearanceProduct
BuildPlanetaryAppearance(
    const terrain::TerrainSource& source,
    f64 referenceRadiusMeters,
    const AppearanceConfig& config = {});

struct GpuAppearanceTexel
{
    math::Float3 albedoLinear{};
    f32 roughness{0.8F};
    math::Float3 normal{};
    f32 oceanMask{0.0F};
    math::Float3 emissionLinear{};
    f32 iceMask{0.0F};
};

class GpuPlanetaryAppearanceProduct
{
public:
    GpuPlanetaryAppearanceProduct(
        rhi::Device& device,
        const PlanetaryAppearanceProduct& product);

    [[nodiscard]] rhi::Buffer& Buffer() noexcept;
    [[nodiscard]] u32 TexelCount() const noexcept;
    [[nodiscard]] u32 FaceResolution() const noexcept;
    [[nodiscard]] u64 Fingerprint() const noexcept;

private:
    std::unique_ptr<rhi::Buffer> buffer_;
    u32 texelCount_{0};
    u32 faceResolution_{0};
    u64 fingerprint_{0};
};
} // namespace orbit::celestial_appearance
