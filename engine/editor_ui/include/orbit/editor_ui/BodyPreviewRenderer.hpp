#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <memory>

namespace orbit::editor_ui
{
struct PreviewMaterial
{
    math::Float3 baseColor{
        0.11F,
        0.26F,
        0.36F
    };
    f32 roughness{0.65F};
    f32 metallic{0.0F};
    math::Float3 emissionRadiance{};
    f32 emissionGiScale{1.0F};
};

// Lightweight but real editor body/material preview. It renders analytic
// sphere/ellipsoid reference shapes directly with a compact PBR lighting
// model. Terrain-capable views can replace the geometry without changing
// the panel contract.
class BodyPreviewRenderer
{
public:
    BodyPreviewRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);
    ~BodyPreviewRenderer();

    BodyPreviewRenderer(
        const BodyPreviewRenderer&) = delete;
    BodyPreviewRenderer& operator=(
        const BodyPreviewRenderer&) = delete;

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& target,
        u32 width,
        u32 height,
        const universe::BodyShape& shape,
        const render_view::CameraState& camera,
        const PreviewMaterial& material = {});

    void DrawSurfaceData(
        rhi::CommandList& commands,
        rhi::Texture& surfaceBaseRoughness,
        rhi::Texture& surfaceNormalMetallic,
        rhi::Texture& surfaceEmissionClass,
        u32 width,
        u32 height,
        const universe::BodyShape& shape,
        const render_view::CameraState& camera,
        const PreviewMaterial& material = {});

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::editor_ui
