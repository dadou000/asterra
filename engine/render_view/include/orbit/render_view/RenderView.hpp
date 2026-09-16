#pragma once

#include <orbit/frames/FrameGraph.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>
#include <optional>

namespace orbit::render_view
{
struct CameraState
{
    frames::FrameId frame{};
    math::Double3 localPositionMeters{};
    math::Float3 forward{
        0.0F,
        -0.28F,
        1.0F};
    math::Float3 up{
        0.0F,
        1.0F,
        0.0F};
    f32 verticalFovRadians{1.22173048F};
    f32 nearPlaneMeters{0.05F};
    f32 farPlaneMeters{12'000'000.0F};
};

struct ViewRay
{
    math::Double3 origin{};
    math::Double3 direction{};
};

[[nodiscard]] std::optional<ViewRay>
ViewportRay(
    const CameraState& camera,
    u32 width,
    u32 height,
    f32 u,
    f32 v) noexcept;

struct RenderViewDesc
{
    u32 width{1};
    u32 height{1};
};

struct ImportedTargets
{
    render_graph::TextureHandle color;
    render_graph::TextureHandle depth;
    render_graph::TextureHandle picking;
};

class RenderView
{
public:
    RenderView(
        rhi::Device& device,
        const RenderViewDesc& desc);

    void Resize(
        u32 width,
        u32 height);

    [[nodiscard]] u32 Width() const noexcept;
    [[nodiscard]] u32 Height() const noexcept;

    [[nodiscard]] CameraState& Camera() noexcept;
    [[nodiscard]] const CameraState& Camera() const noexcept;

    [[nodiscard]] rhi::Texture& Color() noexcept;
    [[nodiscard]] rhi::Texture& Depth() noexcept;
    [[nodiscard]] rhi::Texture& Picking() noexcept;

    // A completed view frame leaves color/picking sampleable and depth
    // in DepthWrite. New graphs import those exact states.
    [[nodiscard]] ImportedTargets Import(
        render_graph::RenderGraph& graph,
        const char* namePrefix);

private:
    void CreateTargets();

    rhi::Device& device_;
    u32 width_{1};
    u32 height_{1};
    CameraState camera_{};
    std::unique_ptr<rhi::Texture> color_;
    std::unique_ptr<rhi::Texture> depth_;
    std::unique_ptr<rhi::Texture> picking_;
};

class CompositeRenderer
{
public:
    CompositeRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& source,
        rhi::Texture& target,
        u32 targetWidth,
        u32 targetHeight);

private:
    std::unique_ptr<rhi::GraphicsPipeline>
        pipeline_;
};
} // namespace orbit::render_view
