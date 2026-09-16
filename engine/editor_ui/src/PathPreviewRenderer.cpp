#include <orbit/editor_ui/PathPreviewRenderer.hpp>

#include <orbit/rhi/Pipeline.hpp>
#include <orbit/rhi/Resource.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace orbit::editor_ui
{
namespace
{
struct RenderVertex
{
    math::Float3 position{};
    math::Float4 color{};
};

constexpr const char* kVertexShader = R"(
struct Push
{
    float4 projection;
    float4 forward;
    float4 up;
};
[[vk::push_constant]] Push g_push;

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    const float3 forward =
        normalize(g_push.forward.xyz);
    const float3 requestedUp =
        normalize(g_push.up.xyz);
    const float3 right =
        normalize(cross(forward, requestedUp));
    const float3 cameraUp =
        normalize(cross(right, forward));

    const float z =
        dot(input.position, forward);

    if (z <= g_push.projection.z ||
        z >= g_push.projection.w)
    {
        output.position =
            float4(2.0, 2.0, 1.0, 1.0);
        output.color = input.color;
        return output;
    }

    const float x =
        dot(input.position, right);
    const float y =
        dot(input.position, cameraUp);
    const float aspect =
        max(g_push.projection.x, 0.001);
    const float tanHalfFov =
        max(g_push.projection.y, 0.001);

    output.position =
        float4(
            x / (aspect * tanHalfFov),
            -y / tanHalfFov,
            z * 0.5,
            z);
    output.color = input.color;
    return output;
}
)";

constexpr const char* kPixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

float4 main(VSOutput input) : SV_Target0
{
    return input.color;
}
)";

[[nodiscard]] math::Float4 MeshColor() noexcept
{
    return {
        0.10F,
        0.70F,
        0.95F,
        0.72F
    };
}

[[nodiscard]] math::Float4 DebugColor(
    const path_geometry::DebugLineKind kind)
    noexcept
{
    switch (kind)
    {
    case path_geometry::DebugLineKind::Boundary:
        return {
            1.0F,
            0.72F,
            0.18F,
            1.0F
        };
    case path_geometry::DebugLineKind::Lane:
        return {
            0.18F,
            0.88F,
            1.0F,
            1.0F
        };
    case path_geometry::DebugLineKind::Centerline:
        return {
            1.0F,
            1.0F,
            1.0F,
            1.0F
        };
    }

    return {
        1.0F,
        1.0F,
        1.0F,
        1.0F
    };
}
} // namespace

class PathPreviewRenderer::Impl
{
public:
    Impl(
        rhi::Device& device,
        const shader::Compiler& compiler)
        : device(device)
    {
        const shader::Binary vertex =
            compiler.Compile({
                .source = kVertexShader,
                .entryPoint = "main",
                .stage = shader::Stage::Vertex,
                .debug = false
            });

        const shader::Binary pixel =
            compiler.Compile({
                .source = kPixelShader,
                .entryPoint = "main",
                .stage = shader::Stage::Pixel,
                .debug = false
            });

        constexpr std::array<
            rhi::VertexAttribute,
            2> attributes{{
                {
                    .location = 0,
                    .format =
                        rhi::VertexFormat::Float3,
                    .offsetBytes =
                        offsetof(
                            RenderVertex,
                            position)
                },
                {
                    .location = 1,
                    .format =
                        rhi::VertexFormat::Float4,
                    .offsetBytes =
                        offsetof(
                            RenderVertex,
                            color)
                }
            }};

        const auto makePipeline =
            [&](const rhi::PrimitiveTopology
                    topology)
            {
                return device.
                    CreateGraphicsPipeline({
                        .vertexShader = {
                            .data =
                                vertex.
                                    bytecode.
                                    data(),
                            .size =
                                vertex.
                                    bytecode.
                                    size()
                        },
                        .pixelShader = {
                            .data =
                                pixel.
                                    bytecode.
                                    data(),
                            .size =
                                pixel.
                                    bytecode.
                                    size()
                        },
                        .vertexAttributes =
                            attributes,
                        .vertexStrideBytes =
                            sizeof(
                                RenderVertex),
                        .pushConstantDwords =
                            12,
                        .sampledTextures = 0,
                        .topology = topology,
                        .fillMode =
                            rhi::FillMode::Solid,
                        .cullMode =
                            rhi::CullMode::None,
                        .blendMode =
                            rhi::BlendMode::Alpha,
                        .depthTest = false,
                        .depthWrite = false
                    });
            };

        meshPipeline =
            makePipeline(
                rhi::PrimitiveTopology::
                    TriangleList);
        linePipeline =
            makePipeline(
                rhi::PrimitiveTopology::
                    LineList);
    }

    void EnsureMeshBuffers(
        const std::size_t vertices,
        const std::size_t indices)
    {
        if (vertices >
                meshVertexCapacity)
        {
            meshVertexCapacity =
                std::max(
                    vertices,
                    std::max<std::size_t>(
                        meshVertexCapacity * 2U,
                        256U));

            meshVertexBuffer =
                device.CreateBuffer({
                    .sizeBytes =
                        static_cast<u64>(
                            meshVertexCapacity *
                            sizeof(
                                RenderVertex)),
                    .usage =
                        rhi::BufferUsage::
                            Vertex,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            VertexOrConstantBuffer
                });
        }

        if (indices >
                meshIndexCapacity)
        {
            meshIndexCapacity =
                std::max(
                    indices,
                    std::max<std::size_t>(
                        meshIndexCapacity * 2U,
                        512U));

            meshIndexBuffer =
                device.CreateBuffer({
                    .sizeBytes =
                        static_cast<u64>(
                            meshIndexCapacity *
                            sizeof(u32)),
                    .usage =
                        rhi::BufferUsage::
                            Index,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            IndexBuffer
                });
        }
    }

    void EnsureLineBuffer(
        const std::size_t vertices)
    {
        if (vertices <=
            lineVertexCapacity)
        {
            return;
        }

        lineVertexCapacity =
            std::max(
                vertices,
                std::max<std::size_t>(
                    lineVertexCapacity * 2U,
                    256U));

        lineVertexBuffer =
            device.CreateBuffer({
                .sizeBytes =
                    static_cast<u64>(
                        lineVertexCapacity *
                        sizeof(RenderVertex)),
                .usage =
                    rhi::BufferUsage::Vertex,
                .memory =
                    rhi::MemoryUsage::
                        HostVisible,
                .initialState =
                    rhi::ResourceState::
                        VertexOrConstantBuffer
            });
    }

    rhi::Device& device;
    std::unique_ptr<rhi::GraphicsPipeline>
        meshPipeline;
    std::unique_ptr<rhi::GraphicsPipeline>
        linePipeline;

    std::unique_ptr<rhi::Buffer>
        meshVertexBuffer;
    std::unique_ptr<rhi::Buffer>
        meshIndexBuffer;
    std::unique_ptr<rhi::Buffer>
        lineVertexBuffer;

    std::size_t meshVertexCapacity{0};
    std::size_t meshIndexCapacity{0};
    std::size_t lineVertexCapacity{0};

    std::vector<RenderVertex>
        meshVertices;
    std::vector<u32> meshIndices;
    std::vector<RenderVertex>
        lineVertices;
};

PathPreviewRenderer::PathPreviewRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : impl_(
          std::make_unique<Impl>(
              device,
              compiler))
{
}

PathPreviewRenderer::~PathPreviewRenderer() =
    default;

void PathPreviewRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const render_view::CameraState& camera,
    const frames::FrameGraph& frames,
    const time::SimulationTime atTime,
    const std::span<
        const path_geometry::
            PathDerivedProduct* const>
        products,
    const bool drawDebugLines)
{
    if (width == 0U ||
        height == 0U ||
        !camera.frame ||
        products.empty())
    {
        return;
    }

    impl_->meshVertices.clear();
    impl_->meshIndices.clear();
    impl_->lineVertices.clear();

    const frames::FramePoint cameraOrigin{
        .frame = camera.frame,
        .localMeters =
            camera.localPositionMeters
    };

    for (const auto* product :
         products)
    {
        if (product == nullptr ||
            product->visualMesh.
                vertices.empty())
        {
            continue;
        }

        std::vector<math::Float3>
            relativeVertices;

        relativeVertices.reserve(
            product->visualMesh.
                vertices.size());

        bool connected = true;

        for (const auto& vertex :
             product->visualMesh.vertices)
        {
            const auto relative =
                frames.ToCameraRelative(
                    {
                        .frame =
                            product->frame,
                        .localMeters =
                            vertex.position
                    },
                    cameraOrigin,
                    atTime);

            if (!relative.has_value())
            {
                connected = false;
                break;
            }

            relativeVertices.push_back(
                *relative);
        }

        if (!connected)
        {
            continue;
        }

        const u32 vertexBase =
            static_cast<u32>(
                impl_->meshVertices.size());

        for (const auto& position :
             relativeVertices)
        {
            impl_->meshVertices.push_back({
                .position = position,
                .color = MeshColor()
            });
        }

        for (const u32 index :
             product->visualMesh.indices)
        {
            impl_->meshIndices.push_back(
                vertexBase + index);
        }

        if (!drawDebugLines)
        {
            continue;
        }

        for (const auto& line :
             product->debugLines)
        {
            const auto a =
                frames.ToCameraRelative(
                    {
                        .frame =
                            product->frame,
                        .localMeters =
                            line.start
                    },
                    cameraOrigin,
                    atTime);
            const auto b =
                frames.ToCameraRelative(
                    {
                        .frame =
                            product->frame,
                        .localMeters =
                            line.end
                    },
                    cameraOrigin,
                    atTime);

            if (!a.has_value() ||
                !b.has_value())
            {
                continue;
            }

            const math::Float4 color =
                DebugColor(line.kind);

            impl_->lineVertices.push_back({
                .position = *a,
                .color = color
            });

            impl_->lineVertices.push_back({
                .position = *b,
                .color = color
            });
        }
    }

    if (impl_->meshVertices.empty() &&
        impl_->lineVertices.empty())
    {
        return;
    }

    const f32 tanHalfFov =
        std::tan(
            camera.verticalFovRadians *
            0.5F);

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(
                value);
        };

    const std::array<u32, 12>
        constants{
            bits(
                static_cast<f32>(width) /
                static_cast<f32>(height)),
            bits(tanHalfFov),
            bits(
                camera.nearPlaneMeters),
            bits(
                camera.farPlaneMeters),

            bits(camera.forward.x),
            bits(camera.forward.y),
            bits(camera.forward.z),
            bits(0.0F),

            bits(camera.up.x),
            bits(camera.up.y),
            bits(camera.up.z),
            bits(0.0F)
        };

    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width =
            static_cast<f32>(width),
        .height =
            static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    });
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right =
            static_cast<i32>(width),
        .bottom =
            static_cast<i32>(height)
    });

    if (!impl_->meshVertices.empty() &&
        !impl_->meshIndices.empty())
    {
        impl_->EnsureMeshBuffers(
            impl_->meshVertices.size(),
            impl_->meshIndices.size());

        std::memcpy(
            impl_->meshVertexBuffer->Map(),
            impl_->meshVertices.data(),
            impl_->meshVertices.size() *
                sizeof(RenderVertex));
        impl_->meshVertexBuffer->Unmap();

        std::memcpy(
            impl_->meshIndexBuffer->Map(),
            impl_->meshIndices.data(),
            impl_->meshIndices.size() *
                sizeof(u32));
        impl_->meshIndexBuffer->Unmap();

        commands.SetGraphicsPipeline(
            *impl_->meshPipeline);
        commands.SetGraphicsConstants(
            constants);
        commands.SetVertexBuffer(
            *impl_->meshVertexBuffer,
            sizeof(RenderVertex));
        commands.SetIndexBuffer(
            *impl_->meshIndexBuffer,
            rhi::IndexFormat::UInt32);
        commands.DrawIndexed(
            static_cast<u32>(
                impl_->meshIndices.
                    size()));
    }

    if (drawDebugLines &&
        !impl_->lineVertices.empty())
    {
        impl_->EnsureLineBuffer(
            impl_->lineVertices.size());

        std::memcpy(
            impl_->lineVertexBuffer->Map(),
            impl_->lineVertices.data(),
            impl_->lineVertices.size() *
                sizeof(RenderVertex));
        impl_->lineVertexBuffer->Unmap();

        commands.SetGraphicsPipeline(
            *impl_->linePipeline);
        commands.SetGraphicsConstants(
            constants);
        commands.SetVertexBuffer(
            *impl_->lineVertexBuffer,
            sizeof(RenderVertex));
        commands.Draw(
            static_cast<u32>(
                impl_->lineVertices.
                    size()));
    }
}
} // namespace orbit::editor_ui
