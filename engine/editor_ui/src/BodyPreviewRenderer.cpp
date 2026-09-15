#include <orbit/editor_ui/BodyPreviewRenderer.hpp>

#include <orbit/rhi/Pipeline.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <type_traits>

namespace orbit::editor_ui
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

    VSOutput output;
    output.position =
        float4(
            positions[vertexId],
            0.0,
            1.0);
    output.uv =
        positions[vertexId];
    return output;
}
)";

constexpr const char* kPixelShader = R"(
struct Constants
{
    float4 radiiAndAspect;
};
[[vk::push_constant]] Constants g_pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target0
{
    float2 p = input.uv;
    p.x *= g_pc.radiiAndAspect.w;

    const float3 radii =
        max(
            g_pc.radiiAndAspect.xyz,
            float3(0.001, 0.001, 0.001));

    const float3 camera =
        float3(0.0, 0.0, -3.2);

    const float3 ray =
        normalize(
            float3(
                p.x,
                -p.y,
                1.8));

    const float3 ro =
        camera / radii;
    const float3 rd =
        ray / radii;

    const float a = dot(rd, rd);
    const float b = 2.0 * dot(ro, rd);
    const float c = dot(ro, ro) - 1.0;
    const float discriminant =
        b * b - 4.0 * a * c;

    if (discriminant < 0.0)
    {
        const float glow =
            0.025 /
            max(
                dot(p, p),
                0.08);

        return float4(
            0.006 + glow * 0.04,
            0.010 + glow * 0.08,
            0.018 + glow * 0.12,
            1.0);
    }

    const float t =
        (-b - sqrt(discriminant)) /
        (2.0 * a);

    const float3 hit =
        camera + ray * t;

    const float3 normal =
        normalize(
            float3(
                hit.x /
                    (radii.x * radii.x),
                hit.y /
                    (radii.y * radii.y),
                hit.z /
                    (radii.z * radii.z)));

    const float3 light =
        normalize(
            float3(
                -0.55,
                0.65,
                -0.70));

    const float diffuse =
        saturate(
            dot(normal, -light));

    const float rim =
        pow(
            1.0 -
                saturate(
                    dot(
                        normal,
                        -ray)),
            3.0);

    const float3 base =
        float3(
            0.11,
            0.26,
            0.36);

    const float3 color =
        base *
            (0.18 +
             diffuse * 0.95) +
        float3(
            0.08,
            0.24,
            0.42) *
            rim;

    return float4(color, 1.0);
}
)";
} // namespace

class BodyPreviewRenderer::Impl
{
public:
    Impl(
        rhi::Device& device,
        const shader::Compiler& compiler)
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

        pipeline =
            device.CreateGraphicsPipeline({
                .vertexShader = {
                    .data =
                        vertex.bytecode.data(),
                    .size =
                        vertex.bytecode.size()
                },
                .pixelShader = {
                    .data =
                        pixel.bytecode.data(),
                    .size =
                        pixel.bytecode.size()
                },
                .vertexAttributes = {},
                .vertexStrideBytes = 0,
                .pushConstantDwords = 4,
                .sampledTextures = 0,
                .topology =
                    rhi::PrimitiveTopology::
                        TriangleList,
                .fillMode =
                    rhi::FillMode::Solid,
                .cullMode =
                    rhi::CullMode::None,
                .depthTest = false,
                .depthWrite = false
            });
    }

    std::unique_ptr<rhi::GraphicsPipeline>
        pipeline;
};

BodyPreviewRenderer::BodyPreviewRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : impl_(
          std::make_unique<Impl>(
              device,
              compiler))
{
}

BodyPreviewRenderer::~BodyPreviewRenderer() = default;

void BodyPreviewRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const universe::BodyShape& shape)
{
    if (width == 0 || height == 0)
    {
        return;
    }

    universe::EllipsoidShape ellipsoid =
        std::visit(
            [](const auto& value)
            {
                using Shape =
                    std::decay_t<
                        decltype(value)>;

                if constexpr (
                    std::is_same_v<
                        Shape,
                        universe::SphereShape>)
                {
                    return universe::
                        EllipsoidShape{
                            .radiiMeters = {
                                value.radiusMeters,
                                value.radiusMeters,
                                value.radiusMeters
                            }
                        };
                }
                else
                {
                    return value;
                }
            },
            shape);

    const f64 maximum =
        std::max({
            ellipsoid.radiiMeters.x,
            ellipsoid.radiiMeters.y,
            ellipsoid.radiiMeters.z
        });

    const std::array<u32, 4> constants{
        std::bit_cast<u32>(
            static_cast<f32>(
                ellipsoid.radiiMeters.x /
                maximum)),
        std::bit_cast<u32>(
            static_cast<f32>(
                ellipsoid.radiiMeters.y /
                maximum)),
        std::bit_cast<u32>(
            static_cast<f32>(
                ellipsoid.radiiMeters.z /
                maximum)),
        std::bit_cast<u32>(
            static_cast<f32>(width) /
            static_cast<f32>(height))
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

    commands.SetGraphicsPipeline(
        *impl_->pipeline);
    commands.SetGraphicsConstants(
        constants);
    commands.Draw(6);
}
} // namespace orbit::editor_ui
