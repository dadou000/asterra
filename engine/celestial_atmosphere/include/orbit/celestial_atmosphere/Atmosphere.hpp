#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>

#include <memory>
#include <vector>

namespace orbit::celestial_atmosphere
{
struct AtmosphereParameters
{
    f64 bottomRadiusMeters{6.371e6};
    f64 topRadiusMeters{6.471e6};

    math::Double3 rayleighScatteringPerMeter{
        5.802e-6, 13.558e-6, 33.1e-6};
    f64 rayleighScaleHeightMeters{8000.0};

    math::Double3 mieScatteringPerMeter{
        3.996e-6, 3.996e-6, 3.996e-6};
    math::Double3 mieExtinctionPerMeter{
        4.44e-6, 4.44e-6, 4.44e-6};
    f64 mieScaleHeightMeters{1200.0};
    f64 mieAnisotropy{0.8};

    math::Double3 absorptionExtinctionPerMeter{
        0.650e-6, 1.881e-6, 0.085e-6};
    f64 absorptionCenterHeightMeters{25000.0};
    f64 absorptionHalfWidthMeters{15000.0};

    math::Double3 groundAlbedo{
        0.10, 0.10, 0.10};
};

struct AtmosphereLutConfig
{
    u32 transmittanceWidth{128};
    u32 transmittanceHeight{32};
    u32 multiScatteringWidth{32};
    u32 multiScatteringHeight{16};
    u32 skyViewWidth{128};
    u32 skyViewHeight{64};

    u32 opticalDepthSteps{64};
    u32 multiDirectionSamples{32};
    u32 skyViewSteps{48};
};

struct AtmosphereSample
{
    f64 altitudeMeters{0.0};
    f64 rayleighDensity{0.0};
    f64 mieDensity{0.0};
    f64 absorptionDensity{0.0};
    math::Double3 scatteringPerMeter{};
    math::Double3 extinctionPerMeter{};
};

[[nodiscard]] AtmosphereSample SampleAtmosphere(
    const AtmosphereParameters& parameters,
    f64 radiusMeters);

struct AtmosphereLut2D
{
    u32 width{0};
    u32 height{0};
    std::vector<math::Float4> texels;
    u64 fingerprint{0};

    [[nodiscard]] const math::Float4& At(
        u32 x,
        u32 y) const;
};

struct AtmosphereStaticLuts
{
    AtmosphereLut2D transmittance;
    AtmosphereLut2D multiScattering;
    u64 fingerprint{0};
};

struct SkyViewInput
{
    f64 observerRadiusMeters{6.371e6};
    math::Double3 sunDirectionBody{
        0.0, 0.0, 1.0};
    math::Double3 incidentIrradianceWattsPerSquareMeter{
        1361.0, 1361.0, 1361.0};
};

struct AtmosphereSkyView
{
    AtmosphereLut2D skyView;
    f64 observerRadiusMeters{0.0};
    math::Double3 sunDirectionBody{};
    math::Double3 incidentIrradianceWattsPerSquareMeter{};
    u64 fingerprint{0};
};

[[nodiscard]] u64 AtmosphereFingerprint(
    const AtmosphereParameters& parameters,
    const AtmosphereLutConfig& config = {});

[[nodiscard]] AtmosphereStaticLuts BuildStaticLuts(
    const AtmosphereParameters& parameters,
    const AtmosphereLutConfig& config = {});

[[nodiscard]] AtmosphereSkyView BuildSkyView(
    const AtmosphereParameters& parameters,
    const AtmosphereStaticLuts& staticLuts,
    const SkyViewInput& input,
    const AtmosphereLutConfig& config = {});

class GpuAtmosphereLuts
{
public:
    GpuAtmosphereLuts(
        rhi::Device& device,
        const AtmosphereStaticLuts& staticLuts,
        const AtmosphereSkyView& skyView);

    void EnsureUploaded(
        rhi::CommandList& commands);

    [[nodiscard]] rhi::Texture& Transmittance() noexcept;
    [[nodiscard]] rhi::Texture& MultiScattering() noexcept;
    [[nodiscard]] rhi::Texture& SkyView() noexcept;

    [[nodiscard]] u64 StaticFingerprint() const noexcept;
    [[nodiscard]] u64 SkyFingerprint() const noexcept;

private:
    struct UploadTexture
    {
        std::unique_ptr<rhi::Buffer> staging;
        std::unique_ptr<rhi::Texture> texture;
        bool uploaded{false};
    };

    static UploadTexture CreateTexture(
        rhi::Device& device,
        const AtmosphereLut2D& lut);

    static void EnsureTextureUploaded(
        rhi::CommandList& commands,
        UploadTexture& product);

    UploadTexture transmittance_;
    UploadTexture multiScattering_;
    UploadTexture skyView_;
    u64 staticFingerprint_{0};
    u64 skyFingerprint_{0};
};
} // namespace orbit::celestial_atmosphere
