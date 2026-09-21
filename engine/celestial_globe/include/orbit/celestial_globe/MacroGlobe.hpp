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
#include <vector>

namespace orbit::celestial_globe
{
struct MacroGlobeConfig
{
    u32 faceResolution{33};
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

class MacroGlobeRenderer
{
public:
    MacroGlobeRenderer(
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
        const MacroGlobeLighting& lighting = {});

private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> surfacePipeline_;
};
} // namespace orbit::celestial_globe
