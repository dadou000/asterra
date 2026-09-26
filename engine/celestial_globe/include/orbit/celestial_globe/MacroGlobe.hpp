#pragma once

#include <orbit/celestial_appearance/PlanetaryAppearance.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain/TerrainSource.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <memory>
#include <span>
#include <vector>

namespace orbit::celestial_globe
{
// Legacy Studio code used to change the entire cube-face resolution between
// 33/65/129/257/513 as the planet crossed screen-size thresholds. Those
// global rebuilds are incompatible with the adaptive patch hierarchy: the
// global product is now a stable seed/fallback, while local cube-sphere
// patches carry the actual orbital LOD. Explicit fixed resolutions remain
// available for deterministic tests/tools.
struct MacroGlobeResolution
{
    static constexpr u32 AdaptiveSeedResolution = 129U;

    u32 requested{33U};
    bool fixed{false};

    constexpr MacroGlobeResolution() noexcept = default;
    constexpr MacroGlobeResolution(const u32 value) noexcept
        : requested(value)
    {
    }

    [[nodiscard]] static constexpr MacroGlobeResolution Fixed(
        const u32 value) noexcept
    {
        MacroGlobeResolution result(value);
        result.fixed = true;
        return result;
    }

    [[nodiscard]] constexpr u32 Effective() const noexcept
    {
        return fixed
            ? requested
            : AdaptiveSeedResolution;
    }

    [[nodiscard]] constexpr operator u32() const noexcept
    {
        return Effective();
    }
};

struct MacroGlobeConfig
{
    MacroGlobeResolution faceResolution{};
    f64 footprintScale{1.5};
};

struct MacroGlobeVertex
{
    math::Double3 positionMeters{};
    math::Double3 normal{};
    f64 elevationMeters{0.0};
};

struct MacroGlobeMesh
{
    std::vector<MacroGlobeVertex> vertices;
    std::vector<u32> indices;
    f64 referenceRadiusMeters{1.0};
    f64 minimumRadiusMeters{1.0};
    f64 maximumRadiusMeters{1.0};
    f64 sampleFootprintMeters{1.0};
    u64 sourceRevision{0};
    u64 fingerprint{0};
};

// The implementation in MacroGlobe.cpp is compiled under this legacy symbol;
// the public wrapper below registers the terrain authority for the hybrid
// renderer before forwarding to it. The legacy declaration intentionally has
// no default argument because MacroGlobe.cpp macro-renames the public symbol to
// this name; repeating the default there is ill-formed in MSVC.
[[nodiscard]] u64 LegacyMacroGlobeFingerprint(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const MacroGlobeConfig& config);

[[nodiscard]] u64 MacroGlobeFingerprint(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const MacroGlobeConfig& config = {});

[[nodiscard]] MacroGlobeMesh BuildMacroGlobe(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const MacroGlobeConfig& config = {});

struct GpuMacroGlobeVertex
{
    math::Float3 positionNormalized{};
    math::Float3 normal{};
    math::Float3 albedoLinear{0.18F, 0.18F, 0.18F};
    math::Float3 appearanceNormal{0.0F, 1.0F, 0.0F};
    math::Float4 materialChannels{0.8F, 0.0F, 0.0F, 0.0F};
    math::Float3 emissionLinear{};
};

class GpuMacroGlobeProduct
{
public:
    GpuMacroGlobeProduct(
        rhi::Device& device,
        const MacroGlobeMesh& mesh,
        const celestial_appearance::PlanetaryAppearanceProduct*
            appearance = nullptr);

    // Patch meshes do not have the six-identical-face topology of the legacy
    // globe. This overload accepts one appearance value per patch vertex.
    GpuMacroGlobeProduct(
        rhi::Device& device,
        const MacroGlobeMesh& mesh,
        std::span<const celestial_appearance::AppearanceTexel> appearance);

    [[nodiscard]] rhi::Buffer& VertexBuffer() noexcept;
    [[nodiscard]] rhi::Buffer& IndexBuffer() noexcept;
    [[nodiscard]] u32 IndexCount() const noexcept;
    [[nodiscard]] f64 ReferenceRadiusMeters() const noexcept;
    [[nodiscard]] u64 Fingerprint() const noexcept;

private:
    std::unique_ptr<rhi::Buffer> vertices_;
    std::unique_ptr<rhi::Buffer> indices_;
    u32 indexCount_{0};
    f64 referenceRadiusMeters_{1.0};
    u64 fingerprint_{0};
};

struct MacroGlobeLighting
{
    math::Float3 directionBody{
        0.55F, 0.72F, -0.48F};
    f32 irradianceScale{1.0F};
    f32 oceanRefractiveIndex{1.333F};
    f32 oceanRoughness{0.12F};
    f32 oceanGlintStrength{1.0F};
    bool oceanEnabled{false};
};

// The old renderer remains compiled privately so the adaptive wrapper can use
// its proven Vulkan pipelines for every independently resident patch.
class LegacyMacroGlobeRenderer
{
public:
    LegacyMacroGlobeRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& target,
        u32 width,
        u32 height,
        GpuMacroGlobeProduct& globe,
        const render_view::CameraState& camera,
        f32 opacity = 1.0F,
        const MacroGlobeLighting& lighting = {});

    void DrawSurface(
        rhi::CommandList& commands,
        rhi::Texture& previewColor,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        u32 width,
        u32 height,
        GpuMacroGlobeProduct& globe,
        const render_view::CameraState& camera,
        f32 opacity = 1.0F,
        const MacroGlobeLighting& lighting = {},
        rhi::Texture* depth = nullptr);

private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> surfacePipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> depthSurfacePipeline_;
};

#ifndef ORBIT_BUILD_LEGACY_MACRO_GLOBE_RENDERER
class HybridMacroGlobeRuntime;

class MacroGlobeRenderer
{
public:
    MacroGlobeRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);
    ~MacroGlobeRenderer();

    MacroGlobeRenderer(const MacroGlobeRenderer&) = delete;
    MacroGlobeRenderer& operator=(const MacroGlobeRenderer&) = delete;

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& target,
        u32 width,
        u32 height,
        GpuMacroGlobeProduct& globe,
        const render_view::CameraState& camera,
        f32 opacity = 1.0F,
        const MacroGlobeLighting& lighting = {});

    void DrawSurface(
        rhi::CommandList& commands,
        rhi::Texture& previewColor,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        u32 width,
        u32 height,
        GpuMacroGlobeProduct& globe,
        const render_view::CameraState& camera,
        f32 opacity = 1.0F,
        const MacroGlobeLighting& lighting = {},
        rhi::Texture* depth = nullptr);

private:
    std::unique_ptr<LegacyMacroGlobeRenderer> legacy_;
    std::unique_ptr<HybridMacroGlobeRuntime> hybrid_;
};
#endif
} // namespace orbit::celestial_globe
