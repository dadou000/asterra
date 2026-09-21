#include <orbit/render_view/RenderView.hpp>

#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/rhi/Resource.hpp>

#include <cmath>
#include <stdexcept>
#include <string>

namespace orbit::render_view
{
namespace
{
constexpr const char* kVertexShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 positions[6] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0, -1.0),
        float2( 1.0, -1.0),
        float2(-1.0,  1.0),
        float2( 1.0,  1.0)
    };

    const float2 uvs[6] =
    {
        float2(0.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 1.0),
        float2(1.0, 1.0),
        float2(0.0, 0.0),
        float2(1.0, 0.0)
    };

    VSOutput output;
    output.position =
        float4(
            positions[vertexId],
            0.0,
            1.0);
    output.uv = uvs[vertexId];
    return output;
}
)";

constexpr const char* kPixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_source;

[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sampler;

float4 main(VSOutput input) : SV_Target0
{
    return g_source.Sample(
        g_sampler,
        input.uv);
}
)";
} // namespace

std::optional<ViewRay>
ViewportRay(
    const CameraState& camera,
    const u32 width,
    const u32 height,
    const f32 u,
    const f32 v) noexcept
{
    if (width == 0 ||
        height == 0 ||
        !std::isfinite(u) ||
        !std::isfinite(v) ||
        u < 0.0F ||
        u > 1.0F ||
        v < 0.0F ||
        v > 1.0F ||
        !std::isfinite(
            camera.verticalFovRadians) ||
        camera.verticalFovRadians <= 0.0F ||
        camera.verticalFovRadians >=
            3.14159265F)
    {
        return std::nullopt;
    }

    const math::Double3 forward =
        math::Normalize(
            math::Double3{
                camera.forward.x,
                camera.forward.y,
                camera.forward.z
            });
    const math::Double3 requestedUp =
        math::Normalize(
            math::Double3{
                camera.up.x,
                camera.up.y,
                camera.up.z
            });
    // Orbit view space is right-handed with +Z forward and +Y up.
    // up x forward therefore yields +X screen-right. Using forward x up
    // mirrors horizontal viewport picking.
    const math::Double3 right =
        math::Normalize(
            math::Cross(
                requestedUp,
                forward));

    if (math::LengthSquared(forward) <=
            1.0e-20 ||
        math::LengthSquared(right) <=
            1.0e-20)
    {
        return std::nullopt;
    }

    const math::Double3 cameraUp =
        math::Normalize(
            math::Cross(
                forward,
                right));

    const f64 aspect =
        static_cast<f64>(width) /
        static_cast<f64>(height);
    const f64 tanHalfFov =
        std::tan(
            static_cast<f64>(
                camera.verticalFovRadians) *
            0.5);
    const f64 pX =
        static_cast<f64>(u) * 2.0 -
        1.0;
    const f64 pY =
        static_cast<f64>(v) * 2.0 -
        1.0;

    const math::Double3 direction =
        math::Normalize(
            forward +
            right *
                (pX *
                 aspect *
                 tanHalfFov) -
            cameraUp *
                (pY *
                 tanHalfFov));

    if (math::LengthSquared(direction) <=
        1.0e-20)
    {
        return std::nullopt;
    }

    return ViewRay{
        .origin =
            camera.localPositionMeters,
        .direction =
            direction
    };
}

RenderView::RenderView(
    rhi::Device& device,
    const RenderViewDesc& desc)
    : device_(device),
      width_(desc.width),
      height_(desc.height)
{
    if (width_ == 0 || height_ == 0)
    {
        throw std::invalid_argument(
            "RenderView dimensions must be non-zero.");
    }

    CreateTargets();
}

void RenderView::CreateTargets()
{
    color_ =
        device_.CreateTexture({
            .width = width_,
            .height = height_,
            .format =
                rhi::TextureFormat::
                    RGBA16_Float,
            .initialState =
                rhi::ResourceState::
                    ShaderResource
        });

    surfaceBaseRoughness_ =
        device_.CreateTexture({
            .width = width_,
            .height = height_,
            .format =
                rhi::TextureFormat::
                    RGBA16_Float,
            .initialState =
                rhi::ResourceState::
                    ShaderResource
        });

    surfaceNormalMetallic_ =
        device_.CreateTexture({
            .width = width_,
            .height = height_,
            .format =
                rhi::TextureFormat::
                    RGBA16_Float,
            .initialState =
                rhi::ResourceState::
                    ShaderResource
        });

    surfaceEmissionClass_ =
        device_.CreateTexture({
            .width = width_,
            .height = height_,
            .format =
                rhi::TextureFormat::
                    RGBA16_Float,
            .initialState =
                rhi::ResourceState::
                    ShaderResource
        });

    displayLinear_ =
        device_.CreateTexture({
            .width = width_,
            .height = height_,
            .format =
                rhi::TextureFormat::
                    RGBA16_Float,
            .initialState =
                rhi::ResourceState::
                    ShaderResource
        });

    displayColor_ =
        device_.CreateTexture({
            .width = width_,
            .height = height_,
            .format =
                rhi::TextureFormat::
                    RGBA8_UNorm,
            .initialState =
                rhi::ResourceState::
                    ShaderResource
        });

    depth_ =
        device_.CreateTexture({
            .width = width_,
            .height = height_,
            .format =
                rhi::TextureFormat::
                    D32_Float,
            .initialState =
                rhi::ResourceState::
                    DepthWrite
        });

    // Object picking uses a dedicated RGBA8 target in V0.0.3. IDs are
    // encoded into color channels by picking passes; this keeps the RHI
    // format surface small until integer render-target formats are added
    // for a concrete renderer requirement.
    picking_ =
        device_.CreateTexture({
            .width = width_,
            .height = height_,
            .format =
                rhi::TextureFormat::
                    RGBA8_UNorm,
            .initialState =
                rhi::ResourceState::
                    ShaderResource
        });
}

void RenderView::Resize(
    const u32 width,
    const u32 height)
{
    if (width == 0 || height == 0)
    {
        throw std::invalid_argument(
            "RenderView dimensions must be non-zero.");
    }

    if (width == width_ &&
        height == height_)
    {
        return;
    }

    width_ = width;
    height_ = height;
    CreateTargets();
}

u32 RenderView::Width() const noexcept
{
    return width_;
}

u32 RenderView::Height() const noexcept
{
    return height_;
}

CameraState& RenderView::Camera() noexcept
{
    return camera_;
}

const CameraState&
RenderView::Camera() const noexcept
{
    return camera_;
}

void RenderView::SetSurfaceDebugMode(
    const lighting::SurfaceDebugMode mode) noexcept
{
    surfaceDebugMode_ = mode;
}

lighting::SurfaceDebugMode
RenderView::SurfaceDebugMode() const noexcept
{
    return surfaceDebugMode_;
}

lighting::LightingView& RenderView::Lighting() noexcept
{
    return lightingView_;
}

const lighting::LightingView&
RenderView::Lighting() const noexcept
{
    return lightingView_;
}

rhi::Texture& RenderView::Color() noexcept
{
    return *color_;
}

rhi::Texture& RenderView::SurfaceBaseRoughness() noexcept
{
    return *surfaceBaseRoughness_;
}

rhi::Texture& RenderView::SurfaceNormalMetallic() noexcept
{
    return *surfaceNormalMetallic_;
}

rhi::Texture& RenderView::SurfaceEmissionClass() noexcept
{
    return *surfaceEmissionClass_;
}

rhi::Texture& RenderView::DisplayLinear() noexcept
{
    return *displayLinear_;
}

rhi::Texture& RenderView::DisplayColor() noexcept
{
    return *displayColor_;
}

rhi::Texture& RenderView::Depth() noexcept
{
    return *depth_;
}

rhi::Texture& RenderView::Picking() noexcept
{
    return *picking_;
}

ImportedTargets RenderView::Import(
    render_graph::RenderGraph& graph,
    const char* namePrefix)
{
    if (namePrefix == nullptr ||
        namePrefix[0] == '\0')
    {
        throw std::invalid_argument(
            "RenderView import requires a name prefix.");
    }

    const std::string prefix(
        namePrefix);

    return {
        .color =
            graph.ImportTexture(
                prefix + ".Color",
                *color_,
                rhi::ResourceState::
                    ShaderResource),
        .surfaceBaseRoughness =
            graph.ImportTexture(
                prefix + ".SurfaceBaseRoughness",
                *surfaceBaseRoughness_,
                rhi::ResourceState::
                    ShaderResource),
        .surfaceNormalMetallic =
            graph.ImportTexture(
                prefix + ".SurfaceNormalMetallic",
                *surfaceNormalMetallic_,
                rhi::ResourceState::
                    ShaderResource),
        .surfaceEmissionClass =
            graph.ImportTexture(
                prefix + ".SurfaceEmissionClass",
                *surfaceEmissionClass_,
                rhi::ResourceState::
                    ShaderResource),
        .displayLinear =
            graph.ImportTexture(
                prefix + ".DisplayLinear",
                *displayLinear_,
                rhi::ResourceState::
                    ShaderResource),
        .display =
            graph.ImportTexture(
                prefix + ".Display",
                *displayColor_,
                rhi::ResourceState::
                    ShaderResource),
        .depth =
            graph.ImportTexture(
                prefix + ".Depth",
                *depth_,
                rhi::ResourceState::
                    DepthWrite),
        .picking =
            graph.ImportTexture(
                prefix + ".Picking",
                *picking_,
                rhi::ResourceState::
                    ShaderResource)
    };
}

CompositeRenderer::CompositeRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const shader::Binary vertex =
        compiler.Compile({
            .source = kVertexShader,
            .entryPoint = "main",
            .stage =
                shader::Stage::Vertex,
            .debug = false
        });

    const shader::Binary pixel =
        compiler.Compile({
            .source = kPixelShader,
            .entryPoint = "main",
            .stage =
                shader::Stage::Pixel,
            .debug = false
        });

    pipeline_ =
        device.CreateGraphicsPipeline({
            .vertexShader = {
                .data = vertex.bytecode.data(),
                .size = vertex.bytecode.size()
            },
            .pixelShader = {
                .data = pixel.bytecode.data(),
                .size = pixel.bytecode.size()
            },
            .vertexAttributes = {},
            .vertexStrideBytes = 0,
            .pushConstantDwords = 0,
            .shaderResourceBuffers = 0,
            .sampledTextures = 1,
            .topology =
                rhi::PrimitiveTopology::
                    TriangleList,
            .fillMode =
                rhi::FillMode::Solid,
            .cullMode =
                rhi::CullMode::None,
            .depthTest = false,
            .depthWrite = false,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 1U
        });
}

void CompositeRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& source,
    rhi::Texture& target,
    const u32 targetWidth,
    const u32 targetHeight)
{
    if (targetWidth == 0 ||
        targetHeight == 0)
    {
        return;
    }

    commands.SetRenderTarget(target);

    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width =
            static_cast<f32>(
                targetWidth),
        .height =
            static_cast<f32>(
                targetHeight),
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    });

    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right =
            static_cast<i32>(
                targetWidth),
        .bottom =
            static_cast<i32>(
                targetHeight)
    });

    commands.SetGraphicsPipeline(
        *pipeline_);

    commands.SetGraphicsTexture(
        0,
        source);

    commands.Draw(6);
}
} // namespace orbit::render_view
