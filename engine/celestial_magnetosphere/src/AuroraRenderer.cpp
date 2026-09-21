#include <orbit/celestial_magnetosphere_render/AuroraRenderer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <stdexcept>

namespace orbit::celestial_magnetosphere_render
{
using celestial_magnetosphere::AuroraVertex;
GpuAuroraMeshProduct::GpuAuroraMeshProduct(
    rhi::Device& device,
    const AuroraMeshProduct& product)
    : indexCount_(
          static_cast<u32>(
              product.indices.size())),
      referenceRadiusMeters_(
          product.referenceRadiusMeters),
      fingerprint_(product.fingerprint)
{
    if(product.vertices.empty() ||
       product.indices.empty() ||
       product.referenceRadiusMeters<=0.0)
    {
        throw std::invalid_argument(
            "GPU aurora mesh requires a valid product.");
    }

    vertices_=
        device.CreateBuffer({
            .sizeBytes=
                static_cast<u64>(
                    product.vertices.size()*
                    sizeof(AuroraVertex)),
            .usage=
                rhi::BufferUsage::Vertex,
            .memory=
                rhi::MemoryUsage::HostVisible,
            .initialState=
                rhi::ResourceState::
                    VertexOrConstantBuffer
        });

    indices_=
        device.CreateBuffer({
            .sizeBytes=
                static_cast<u64>(
                    product.indices.size()*
                    sizeof(u32)),
            .usage=
                rhi::BufferUsage::Index,
            .memory=
                rhi::MemoryUsage::HostVisible,
            .initialState=
                rhi::ResourceState::
                    IndexBuffer
        });

    if(!vertices_ || !indices_)
    {
        throw std::runtime_error(
            "Failed to allocate aurora GPU buffers.");
    }

    std::memcpy(
        vertices_->Map(),
        product.vertices.data(),
        product.vertices.size()*
            sizeof(AuroraVertex));
    vertices_->Unmap();

    std::memcpy(
        indices_->Map(),
        product.indices.data(),
        product.indices.size()*
            sizeof(u32));
    indices_->Unmap();
}

rhi::Buffer&
GpuAuroraMeshProduct::VertexBuffer() noexcept
{
    return *vertices_;
}

rhi::Buffer&
GpuAuroraMeshProduct::IndexBuffer() noexcept
{
    return *indices_;
}

u32
GpuAuroraMeshProduct::IndexCount() const noexcept
{
    return indexCount_;
}

f64
GpuAuroraMeshProduct::ReferenceRadiusMeters() const noexcept
{
    return referenceRadiusMeters_;
}

u64
GpuAuroraMeshProduct::Fingerprint() const noexcept
{
    return fingerprint_;
}

namespace
{
constexpr const char* kVs=R"(
struct VSIn
{
    float3 position : POSITION;
    float3 emission : COLOR0;
    float2 presentation : TEXCOORD0;
};

struct Constants
{
    float4 cameraAndAspect;
    float4 forwardAndTanHalfFov;
    float4 upAndScale;
    float4 presentation;
};
[[vk::push_constant]] Constants g;

struct VSOut
{
    float4 position : SV_Position;
    float3 emission : COLOR0;
    float opacity : TEXCOORD0;
    float3 bodyPosition : TEXCOORD1;
};

VSOut main(VSIn i)
{
    const float3 forward=
        normalize(
            g.forwardAndTanHalfFov.xyz);
    const float3 requestedUp=
        normalize(
            g.upAndScale.xyz);
    const float3 right=
        normalize(
            cross(
                forward,
                requestedUp));
    const float3 up=
        normalize(
            cross(
                right,
                forward));

    const float3 camera=
        g.cameraAndAspect.xyz;
    const float3 world=
        i.position*
        g.upAndScale.w;
    const float3 relative=
        world-camera;

    const float z=
        dot(relative,forward);
    const float x=
        dot(relative,right);
    const float y=
        dot(relative,up);
    const float tanHalf=
        max(
            g.forwardAndTanHalfFov.w,
            0.001);
    const float aspect=
        max(
            g.cameraAndAspect.w,
            0.001);

    VSOut o;
    o.position=
        float4(
            x/(tanHalf*aspect),
            y/tanHalf,
            z*0.5,
            z);
    o.emission=
        i.emission*
        max(g.presentation.x,0.0);
    o.opacity=
        saturate(
            i.presentation.x*
            max(g.presentation.y,0.0));
    o.bodyPosition=
        i.position;
    return o;
}
)";

constexpr const char* kPs=R"(
struct Constants
{
    float4 cameraAndAspect;
    float4 forwardAndTanHalfFov;
    float4 upAndScale;
    float4 presentation;
};
[[vk::push_constant]] Constants g;

struct VSOut
{
    float4 position : SV_Position;
    float3 emission : COLOR0;
    float opacity : TEXCOORD0;
    float3 bodyPosition : TEXCOORD1;
};

float4 main(VSOut i) : SV_Target0
{
    const float radial=
        length(i.bodyPosition);
    const float bodyOcclusionRadius=
        max(g.presentation.z,0.0);

    if(radial<bodyOcclusionRadius)
        discard;

    // Treat the curtain as a thin emissive volume. Edge-on views remain
    // visible without inventing a surface normal or diffuse lighting term.
    const float heightFade=
        saturate(
            1.0-
            abs(
                radial-
                g.presentation.w)/
            max(
                g.presentation.w*
                0.35,
                1e-4));

    const float alpha=
        saturate(
            i.opacity*
            (0.45+
             0.55*heightFade));

    return float4(
        max(i.emission,0.0),
        alpha);
}
)";
}

AuroraRenderer::AuroraRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vs=
        compiler.Compile({
            .source=kVs,
            .entryPoint="main",
            .stage=shader::Stage::Vertex,
            .debug=false
        });

    const auto ps=
        compiler.Compile({
            .source=kPs,
            .entryPoint="main",
            .stage=shader::Stage::Pixel,
            .debug=false
        });

    static constexpr
        std::array<
            rhi::VertexAttribute,
            3>
        attributes{{
            {
                .location=0,
                .format=
                    rhi::VertexFormat::Float3,
                .offsetBytes=
                    static_cast<u32>(
                        offsetof(
                            AuroraVertex,
                            positionNormalized))
            },
            {
                .location=1,
                .format=
                    rhi::VertexFormat::Float3,
                .offsetBytes=
                    static_cast<u32>(
                        offsetof(
                            AuroraVertex,
                            emissionLinear))
            },
            {
                .location=2,
                .format=
                    rhi::VertexFormat::Float2,
                .offsetBytes=
                    static_cast<u32>(
                        offsetof(
                            AuroraVertex,
                            presentation))
            }
        }};

    pipeline_=
        device.CreateGraphicsPipeline({
            .vertexShader={
                .data=vs.bytecode.data(),
                .size=vs.bytecode.size()
            },
            .pixelShader={
                .data=ps.bytecode.data(),
                .size=ps.bytecode.size()
            },
            .vertexAttributes=
                attributes,
            .vertexStrideBytes=
                sizeof(AuroraVertex),
            .pushConstantDwords=16,
            .topology=
                rhi::PrimitiveTopology::
                    TriangleList,
            .fillMode=
                rhi::FillMode::Solid,
            .cullMode=
                rhi::CullMode::None,
            .blendMode=
                rhi::BlendMode::Alpha,
            .depthTest=false,
            .depthWrite=false,
            .colorAttachmentFormats={
                rhi::TextureFormat::
                    RGBA16_Float
            },
            .colorAttachmentCount=1U
        });
}

void AuroraRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    GpuAuroraMeshProduct& mesh,
    const render_view::CameraState& camera,
    const f32 intensityScale)
{
    if(width==0U ||
       height==0U ||
       mesh.IndexCount()==0U)
    {
        return;
    }

    const f64 radius=
        std::max(
            mesh.ReferenceRadiusMeters(),
            1.0);

    const auto bits=[](const f32 v)
    {
        return std::bit_cast<u32>(v);
    };

    const std::array<u32,16>
        constants{
            bits(static_cast<f32>(
                camera.localPositionMeters.x/
                radius)),
            bits(static_cast<f32>(
                camera.localPositionMeters.y/
                radius)),
            bits(static_cast<f32>(
                camera.localPositionMeters.z/
                radius)),
            bits(
                static_cast<f32>(width)/
                static_cast<f32>(
                    std::max(height,1U))),

            bits(camera.forward.x),
            bits(camera.forward.y),
            bits(camera.forward.z),
            bits(
                std::tan(
                    camera.verticalFovRadians*
                    0.5F)),

            bits(camera.up.x),
            bits(camera.up.y),
            bits(camera.up.z),
            bits(1.0F),

            bits(std::max(
                intensityScale,
                0.0F)),
            bits(1.0F),
            bits(1.0F),
            bits(1.0F)
        };

    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x=0.0F,
        .y=0.0F,
        .width=static_cast<f32>(width),
        .height=static_cast<f32>(height),
        .minDepth=0.0F,
        .maxDepth=1.0F
    });
    commands.SetScissor({
        .left=0,
        .top=0,
        .right=static_cast<i32>(width),
        .bottom=static_cast<i32>(height)
    });
    commands.SetGraphicsPipeline(
        *pipeline_);
    commands.SetGraphicsConstants(
        constants);
    commands.SetVertexBuffer(
        mesh.VertexBuffer(),
        sizeof(AuroraVertex));
    commands.SetIndexBuffer(
        mesh.IndexBuffer(),
        rhi::IndexFormat::UInt32);
    commands.DrawIndexed(
        mesh.IndexCount());
}
} // namespace orbit::celestial_magnetosphere_render
