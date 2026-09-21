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
    bool stellar{false};
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
