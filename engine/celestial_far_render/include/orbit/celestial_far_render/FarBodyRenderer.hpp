#pragma once

#include <orbit/celestial_appearance/PlanetaryAppearance.hpp>
#include <orbit/celestial_representation/RepresentationResolver.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <memory>
#include <vector>

namespace orbit::celestial_far_render
{
struct AppearanceSummary
{
    math::Float3 albedoLinear{0.18F, 0.18F, 0.18F};
    f32 roughness{0.8F};
    f32 oceanFraction{0.0F};
    f32 iceFraction{0.0F};
    f32 directLightTransmittance{1.0F};
    math::Float3 emissionLinear{};
};

[[nodiscard]] AppearanceSummary SummarizeAppearance(
    const celestial_appearance::PlanetaryAppearanceProduct& appearance);

struct CachedDiscConfig
{
    u32 resolution{64};
};

struct CachedDiscProduct
{
    u32 resolution{0};
    u64 appearanceFingerprint{0};
    u64 fingerprint{0};
    // Scene-linear half-float RGBA. Far-body emission must retain HDR values
    // until the shared presentation resolve instead of being clamped to UNorm.
    std::vector<u16> rgba16;
};

[[nodiscard]] CachedDiscProduct BuildCachedDisc(
    const celestial_appearance::PlanetaryAppearanceProduct& appearance,
    const CachedDiscConfig& config = {});

class GpuCachedDiscProduct
{
public:
    GpuCachedDiscProduct(
        rhi::Device& device,
        const CachedDiscProduct& product);

    void EnsureUploaded(
        rhi::CommandList& commands);

    [[nodiscard]] rhi::Texture& Texture() noexcept;
    [[nodiscard]] u64 Fingerprint() const noexcept;

private:
    std::unique_ptr<rhi::Buffer> staging_;
    std::unique_ptr<rhi::Texture> texture_;
    u64 fingerprint_{0};
    bool uploaded_{false};
};

struct FarBodyDraw
{
    celestial_representation::Representation representation{
        celestial_representation::Representation::SmoothGlobe};
    universe::BodyShape shape{};
    render_view::CameraState camera{};
    AppearanceSummary appearance{};
    f64 projectedRadiusPixels{1.0};
    f32 opacity{1.0F};
    f32 radiometricIntensity{1.0F};
    math::Float3 lightDirectionBody{
        0.55F, 0.72F, -0.48F};
    f32 incidentLightScale{1.0F};
    f32 oceanRefractiveIndex{1.333F};
    f32 oceanRoughness{0.12F};
    f32 oceanGlintStrength{1.0F};
    bool oceanEnabled{false};

    // M27 appearance-only giant controls. Mutually exclusive with stellar.
    bool giantEnabled{false};
    math::Float3 giantBaseColorLinear{0.62F, 0.48F, 0.31F};
    math::Float3 giantBandColorLinear{0.90F, 0.78F, 0.58F};
    math::Float3 giantPolarColorLinear{0.48F, 0.42F, 0.36F};
    f32 giantBandFrequency{11.0F};
    f32 giantBandStrength{0.72F};
    f32 giantZonalShear{0.18F};
    f32 giantStormStrength{0.35F};
    f32 giantStormScale{5.0F};
    f32 giantPolarStrength{0.22F};
    f32 giantDepthContrast{0.25F};
    f32 giantTurbulenceStrength{0.18F};
    u32 giantSeed{1U};

    // M28 airless / irregular small-body controls. These parameters deform
    // only the derived far representation; the semantic Reference Shape
    // remains the physical scale authority.
    bool smallBodyEnabled{false};
    math::Float3 smallBodyAxisScale{1.0F, 0.82F, 0.68F};
    f32 smallBodyIrregularity{0.18F};
    f32 smallBodyLargeLobeStrength{0.12F};
    f32 smallBodyCraterDensity{0.55F};
    f32 smallBodyCraterDepth{0.12F};
    f32 smallBodyCraterRimStrength{0.08F};
    math::Float3 smallBodyFreshMaterialColorLinear{0.24F, 0.22F, 0.19F};
    f32 smallBodyColorVariation{0.18F};
    f32 smallBodyOppositionStrength{0.55F};
    f32 smallBodyOppositionWidthRadians{0.055F};
    f32 smallBodySingleScatteringAlbedo{0.16F};
    f32 smallBodyMacroscopicRoughnessRadians{0.42F};
    u32 smallBodySeed{1U};

    bool stellar{false};

    // M26 appearance-only stellar controls. They never alter M19 luminosity.
    math::Float3 stellarColorLinear{1.0F, 1.0F, 1.0F};
    f32 stellarLimbDarkening{0.58F};
    f32 stellarGranulationStrength{0.10F};
    f32 stellarGranulationScale{42.0F};
    f32 stellarActivityLevel{0.12F};
    u32 stellarActivitySeed{1U};
    f32 stellarChromosphereStrength{0.08F};
    f32 stellarChromosphereExtent{0.035F};
    f32 stellarCoronaStrength{0.025F};
    f32 stellarCoronaExtent{1.75F};
    f32 stellarGlareStrength{0.35F};
    f32 stellarGlareRadiusPixels{5.0F};
};

class FarBodyRenderer
{
public:
    FarBodyRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& target,
        u32 width,
        u32 height,
        const FarBodyDraw& draw,
        GpuCachedDiscProduct* cachedDisc = nullptr);

    // Emits only lighting-facing material/surface authority. Cached preview
    // color is deliberately not decoded as albedo because it is already lit.
    void DrawSurfaceData(
        rhi::CommandList& commands,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        u32 width,
        u32 height,
        const FarBodyDraw& draw);

private:
    std::unique_ptr<rhi::GraphicsPipeline> analyticPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> cachedPipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> surfacePipeline_;
};
} // namespace orbit::celestial_far_render
