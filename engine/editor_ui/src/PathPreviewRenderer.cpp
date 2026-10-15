#include <orbit/editor_ui/PathPreviewRenderer.hpp>

#include <orbit/core/Log.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/rhi/Resource.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <format>
#include <limits>

namespace orbit::editor_ui
{
namespace
{
struct RenderVertex
{
    math::Float3 position{};
    math::Float4 color{};
    math::Float3 normal{0.0F, 1.0F, 0.0F};
    math::Float2 corridorUv{};
    math::Float2 corridorInfo{}; // half-width, lane count (0 => unmarked)
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
    [[vk::location(2)]] float3 normal : NORMAL0;
    [[vk::location(3)]] float2 corridorUv : TEXCOORD0;
    [[vk::location(4)]] float2 corridorInfo : TEXCOORD1;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float3 normal : TEXCOORD0;
    float2 corridorUv : TEXCOORD1;
    float2 corridorInfo : TEXCOORD2;
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

    // Do not push vertices outside near/far to a fixed off-screen point.
    // A road triangle straddling the near plane would then acquire an
    // enormous artificial edge, producing view-dependent stretched sheets.
    // Always return the homogeneous projection and let the rasterizer clip
    // the entire primitive to -w<=x,y<=w and 0<=z<=w.

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
            // The Vulkan backend already flips the viewport (negative height),
            // so NDC Y is up here exactly as in every other renderer; negating
            // it mirrored path and diagnostic overlays vertically.
            y / tanHalfFov,
            // Reverse-Z finite perspective: near -> 1, far -> 0.
            // Clip-space w=z; avoid a constant clip-space near plane.
            (g_push.projection.z *
                (g_push.projection.w - z)) /
                max(g_push.projection.w - g_push.projection.z, 0.001),
            z);
    output.color = input.color;
    output.normal = input.normal;
    output.corridorUv = input.corridorUv;
    output.corridorInfo = input.corridorInfo;
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

// Pure analytic road material: no textures, no samplers, no extra draws.
// Distances come from swept world-metre UVs, so lines remain metric.
constexpr const char* kSurfacePixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float3 normal : TEXCOORD0;
    float2 corridorUv : TEXCOORD1;
    float2 corridorInfo : TEXCOORD2;
};
struct SurfaceOutput
{
    float4 color : SV_Target0;
    float4 baseRoughness : SV_Target1;
    float4 normalMetallic : SV_Target2;
    float4 emissionClass : SV_Target3;
};
float Hash(float2 p)
{
    return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453);
}
SurfaceOutput main(VSOutput input)
{
    SurfaceOutput output;
    float3 base = input.color.rgb;
    float roughness = 0.86;
    const float halfWidth = input.corridorInfo.x;
    const float lanes = input.corridorInfo.y;
    if(lanes >= 1.0 && halfWidth > 1.0)
    {
        const float s = input.corridorUv.x;
        const float lateral = input.corridorUv.y;
        // Sparse coarse aggregate breakup in physical metres, not UV pixels.
        const float grain = Hash(floor(float2(s,lateral)*7.0));
        const float patches = Hash(floor(float2(s,lateral)*0.48));
        base = float3(0.125,0.135,0.145) *
            (0.90 + grain*0.09 + patches*0.08);
        const float aa = max(fwidth(lateral),0.006);
        const float edgeDist = abs(abs(lateral)-(halfWidth-0.19));
        const float edge = 1.0-smoothstep(0.055-aa,0.065+aa,edgeDist);
        float white = edge;
        float yellow = 0.0;
        const float laneWidth = (2.0*halfWidth)/lanes;
        const float dash = step(frac(s/9.0),0.55);
        for(int k=1;k<6;++k)
        {
            if(float(k)>=lanes) break;
            const float divider = -halfWidth+laneWidth*float(k);
            const float d = abs(lateral-divider);
            const float line = 1.0-smoothstep(0.045-aa,0.065+aa,d);
            if(k*2 == int(lanes))
                yellow = max(yellow,line);
            else
                white = max(white,line*dash);
        }
        base = lerp(base,float3(0.82,0.83,0.78),saturate(white));
        base = lerp(base,float3(0.88,0.67,0.16),saturate(yellow));
        roughness = 0.91;
    }
    output.color = float4(base,1.0);
    output.baseRoughness = float4(base,roughness);
    output.normalMetallic = float4(normalize(input.normal),0.0);
    output.emissionClass = float4(0.0,0.0,0.0,0.0);
    return output;
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

        const shader::Binary surfacePixel =
            compiler.Compile({
                .source = kSurfacePixelShader,
                .entryPoint = "main",
                .stage = shader::Stage::Pixel,
                .debug = false
            });

        constexpr std::array<
            rhi::VertexAttribute,
            5> attributes{{
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
                },
                {
                    .location = 2,
                    .format = rhi::VertexFormat::Float3,
                    .offsetBytes = offsetof(RenderVertex, normal)
                },
                {.location = 3, .format = rhi::VertexFormat::Float2,
                 .offsetBytes = offsetof(RenderVertex, corridorUv)},
                {.location = 4, .format = rhi::VertexFormat::Float2,
                 .offsetBytes = offsetof(RenderVertex, corridorInfo)}
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
                        .blendMode = topology == rhi::PrimitiveTopology::TriangleList
                            ? rhi::BlendMode::Opaque : rhi::BlendMode::Alpha,
                        .depthCompare = rhi::DepthCompare::GreaterEqual,
                        .depthTest = topology == rhi::PrimitiveTopology::TriangleList,
                        .depthWrite = topology == rhi::PrimitiveTopology::TriangleList,
                        .colorAttachmentFormats = {
                            rhi::TextureFormat::RGBA16_Float
                        },
                        .colorAttachmentCount = 1U
                    });
            };

        meshPipeline =
            makePipeline(
                rhi::PrimitiveTopology::
                    TriangleList);
        surfacePipeline = device.CreateGraphicsPipeline({
            .vertexShader = {.data = vertex.bytecode.data(),
                             .size = vertex.bytecode.size()},
            .pixelShader = {.data = surfacePixel.bytecode.data(),
                            .size = surfacePixel.bytecode.size()},
            .vertexAttributes = attributes,
            .vertexStrideBytes = sizeof(RenderVertex),
            .pushConstantDwords = 12,
            .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Opaque,
            .depthCompare = rhi::DepthCompare::GreaterEqual,
            .depthTest = true,
            .depthWrite = true,
            .colorAttachmentFormats = {
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float,
                rhi::TextureFormat::RGBA16_Float
            },
            .colorAttachmentCount = 4U
        });
        linePipeline =
            makePipeline(
                rhi::PrimitiveTopology::
                    LineList);
    }

    struct MeshUpload
    {
        std::unique_ptr<rhi::Buffer> vertexBuffer;
        std::unique_ptr<rhi::Buffer> indexBuffer;
        std::size_t vertexCapacity{0};
        std::size_t indexCapacity{0};
    };

    void EnsureMeshBuffers(
        MeshUpload& upload,
        const std::size_t vertices,
        const std::size_t indices)
    {
        if (vertices > upload.vertexCapacity)
        {
            upload.vertexCapacity =
                std::max(vertices,
                    std::max<std::size_t>(
                        upload.vertexCapacity * 2U, 256U));

            upload.vertexBuffer =
                device.CreateBuffer({
                    .sizeBytes =
                        static_cast<u64>(
                            upload.vertexCapacity *
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

        if (indices > upload.indexCapacity)
        {
            upload.indexCapacity =
                std::max(indices,
                    std::max<std::size_t>(
                        upload.indexCapacity * 2U, 512U));

            upload.indexBuffer =
                device.CreateBuffer({
                    .sizeBytes =
                        static_cast<u64>(
                            upload.indexCapacity *
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
        surfacePipeline;
    std::unique_ptr<rhi::GraphicsPipeline>
        linePipeline;

    // The render graph records multiple viewport road draws before
    // submission. Each view must own its camera-relative VB/IB; otherwise
    // the last viewport's mapped upload overwrites earlier draw commands.
    std::unordered_map<std::string, MeshUpload> meshUploads;
    std::unordered_set<std::string> warnedBadMeshSignatures;
    std::unique_ptr<rhi::Buffer>
        lineVertexBuffer;

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
    const bool drawDebugLines,
    rhi::Texture* const depth,
    const std::span<rhi::Texture* const> surfaceAttachments,
    const std::string_view viewportId)
{
    if (!surfaceAttachments.empty() && surfaceAttachments.size() != 3U)
        throw std::invalid_argument("Corridor MRT requires exactly three surface attachments.");
    if (!surfaceAttachments.empty() && depth == nullptr)
        throw std::invalid_argument("Corridor MRT requires a depth attachment.");
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

        // Index corruption or NaN coordinates must never reach the GPU.
        // Diagnose each bad published revision once, keeping the currently
        // visible good road products intact instead of uploading undefined
        // vertex fetches or poison geometry to the clipper.
        const auto& mesh=product->visualMesh;
        const auto finite3=[](const math::Double3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) &&
                   std::isfinite(v.z);
        };
        const bool invalidIndices=mesh.indices.size()%3U!=0U ||
            std::any_of(mesh.indices.begin(),mesh.indices.end(),
                [&mesh](u32 index){return index>=mesh.vertices.size();});
        const bool nonFiniteVertices=std::any_of(
            mesh.vertices.begin(),mesh.vertices.end(),
            [&](const path_geometry::PathMeshVertex& v)
            {
                return !finite3(v.position) || !finite3(v.normal) ||
                    !std::isfinite(v.uv.x) || !std::isfinite(v.uv.y);
            });
        if(invalidIndices || nonFiniteVertices)
        {
            const auto key=std::format("{}:{:016x}",
                product->edge.ToString(),product->buildSignature);
            if(impl_->warnedBadMeshSignatures.insert(key).second)
                orbit::log::Warning(std::format(
                    "Road mesh rejected: {} ({}). Check source/junction "
                    "publication for corrupt indices or nonfinite vertices.",
                    key,invalidIndices?"out-of-range triangle index":
                        "nonfinite vertex data"));
            continue;
        }

        std::vector<math::Float3> relativeNormals;
        relativeNormals.reserve(product->visualMesh.vertices.size());
        std::vector<math::Float3>
            relativeVertices;

        relativeVertices.reserve(
            product->visualMesh.
                vertices.size());

        // Resolve the rigid frame once per product, rather than twice per
        // vertex. The old normal calculation subtracted two already-rounded
        // float camera-relative coordinates (position and position+normal),
        // losing the one-metre offset at large planetary distances.
        const auto cameraFromProduct=frames.ResolveTransform(
            product->frame,camera.frame,atTime);
        if(!cameraFromProduct)continue;
        for(const auto& vertex:mesh.vertices)
        {
            const auto relative=math::TransformPoint(
                *cameraFromProduct,vertex.position)-
                camera.localPositionMeters;
            auto normal=math::Normalize(math::TransformVector(
                cameraFromProduct->rotation,vertex.normal));
            if(math::LengthSquared(normal)<0.25)
                normal={0.0,1.0,0.0};
            relativeVertices.push_back({
                static_cast<f32>(relative.x),
                static_cast<f32>(relative.y),
                static_cast<f32>(relative.z)});
            relativeNormals.push_back({
                static_cast<f32>(normal.x),
                static_cast<f32>(normal.y),
                static_cast<f32>(normal.z)});
        }

        const u32 vertexBase =
            static_cast<u32>(
                impl_->meshVertices.size());

        for (std::size_t vertexIndex = 0;
             vertexIndex < relativeVertices.size(); ++vertexIndex)
        {
            impl_->meshVertices.push_back({
                .position = relativeVertices[vertexIndex],
                .color = product->laneCount > 0U ? math::Float4{
                    0.14F, 0.15F, 0.16F, 1.0F} : MeshColor(),
                .normal = relativeNormals[vertexIndex],
                .corridorUv = {
                    static_cast<orbit::f32>(product->visualMesh.vertices[vertexIndex].uv.x),
                    static_cast<orbit::f32>(product->visualMesh.vertices[vertexIndex].uv.y)},
                .corridorInfo = {
                    static_cast<orbit::f32>(product->widthMeters*0.5),
                    static_cast<orbit::f32>(product->laneCount)}
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

    if (!surfaceAttachments.empty())
    {
        const std::array<rhi::Texture*, 4> attachments{
            &target, surfaceAttachments[0], surfaceAttachments[1],
            surfaceAttachments[2]};
        commands.SetRenderTargets(attachments, depth);
    }
    else if (depth) commands.SetRenderTargets(target, *depth);
    else commands.SetRenderTarget(target);
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
        // Independent GPU buffers for every viewport render-pass identity.
        // Buffer::Map waits for previous submitted uses of this *view's*
        // buffer; a different viewport cannot replace its contents before
        // this frame's command buffer has even been submitted.
        auto& upload=impl_->meshUploads[std::string(viewportId)];
        impl_->EnsureMeshBuffers(upload,
            impl_->meshVertices.size(), impl_->meshIndices.size());

        std::memcpy(
            upload.vertexBuffer->Map(),
            impl_->meshVertices.data(),
            impl_->meshVertices.size() *
                sizeof(RenderVertex));
        upload.vertexBuffer->Unmap();

        std::memcpy(
            upload.indexBuffer->Map(),
            impl_->meshIndices.data(),
            impl_->meshIndices.size() *
                sizeof(u32));
        upload.indexBuffer->Unmap();

        commands.SetGraphicsPipeline(
            surfaceAttachments.empty() ? *impl_->meshPipeline : *impl_->surfacePipeline);
        commands.SetGraphicsConstants(
            constants);
        commands.SetVertexBuffer(
            *upload.vertexBuffer,
            sizeof(RenderVertex));
        commands.SetIndexBuffer(
            *upload.indexBuffer,
            rhi::IndexFormat::UInt32);
        commands.DrawIndexed(
            static_cast<u32>(
                impl_->meshIndices.
                    size()));
    }

    if (drawDebugLines &&
        !impl_->lineVertices.empty())
    {
        commands.SetRenderTarget(target);
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
void PathPreviewRenderer::DrawCameraRelativeLines(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const render_view::CameraState& camera,
    const std::span<const PreviewLine> lines)
{
    if (width == 0U ||
        height == 0U ||
        lines.empty())
    {
        return;
    }

    impl_->lineVertices.clear();
    impl_->lineVertices.reserve(
        lines.size() * 2U);

    for (const auto& line : lines)
    {
        impl_->lineVertices.push_back({
            .position = line.start,
            .color = line.color
        });
        impl_->lineVertices.push_back({
            .position = line.end,
            .color = line.color
        });
    }

    impl_->EnsureLineBuffer(
        impl_->lineVertices.size());

    std::memcpy(
        impl_->lineVertexBuffer->Map(),
        impl_->lineVertices.data(),
        impl_->lineVertices.size() *
            sizeof(RenderVertex));
    impl_->lineVertexBuffer->Unmap();

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
            bits(camera.nearPlaneMeters),
            bits(camera.farPlaneMeters),

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
        .width = static_cast<f32>(width),
        .height = static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    });
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right = static_cast<i32>(width),
        .bottom = static_cast<i32>(height)
    });
    commands.SetGraphicsPipeline(
        *impl_->linePipeline);
    commands.SetGraphicsConstants(
        constants);
    commands.SetVertexBuffer(
        *impl_->lineVertexBuffer,
        sizeof(RenderVertex));
    commands.Draw(
        static_cast<u32>(
            impl_->lineVertices.size()));
}

} // namespace orbit::editor_ui
