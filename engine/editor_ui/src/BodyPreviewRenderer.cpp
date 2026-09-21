#include <orbit/editor_ui/BodyPreviewRenderer.hpp>

#include <orbit/rhi/Pipeline.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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
    float4 cameraAndTanHalfFov;
    float4 forward;
    float4 up;
    float4 baseColorAndRoughness;
    float4 materialParameters;
};
[[vk::push_constant]] Constants g_pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target0
{
    const float2 p = input.uv;

    const float3 radii =
        max(
            g_pc.radiiAndAspect.xyz,
            float3(0.001, 0.001, 0.001));

    const float3 camera =
        g_pc.cameraAndTanHalfFov.xyz;

    const float3 forward =
        normalize(g_pc.forward.xyz);
    const float3 requestedUp =
        normalize(g_pc.up.xyz);
    const float3 right =
        normalize(
            cross(
                forward,
                requestedUp));
    const float3 cameraUp =
        normalize(
            cross(
                right,
                forward));
    const float tanHalfFov =
        max(
            g_pc.cameraAndTanHalfFov.w,
            0.001);

    const float3 ray =
        normalize(
            forward +
            right *
                (p.x *
                 g_pc.radiiAndAspect.w *
                 tanHalfFov) -
            cameraUp *
                (p.y * tanHalfFov));

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

    const float3 lightDirection =
        normalize(
            float3(
                0.55,
                0.70,
                -0.65));
    const float3 viewDirection =
        normalize(-ray);
    const float3 halfVector =
        normalize(
            lightDirection +
            viewDirection);

    const float nDotL =
        saturate(
            dot(normal, lightDirection));
    const float nDotV =
        saturate(
            dot(normal, viewDirection));
    const float nDotH =
        saturate(
            dot(normal, halfVector));

    const float3 baseColor =
        saturate(
            g_pc.baseColorAndRoughness.xyz);
    const float roughness =
        saturate(
            g_pc.baseColorAndRoughness.w);
    const float metallic =
        saturate(
            g_pc.materialParameters.x);
    const float3 emission =
        max(
            g_pc.materialParameters.yzw,
            0.0);

    const float3 dielectricF0 =
        float3(0.04, 0.04, 0.04);
    const float3 f0 =
        lerp(
            dielectricF0,
            baseColor,
            metallic);

    const float specularPower =
        lerp(
            192.0,
            6.0,
            max(roughness, 0.04));
    const float specularLobe =
        pow(
            nDotH,
            specularPower) *
        lerp(
            1.25,
            0.18,
            roughness);

    const float fresnel =
        pow(
            1.0 - nDotV,
            5.0);
    const float3 specular =
        (f0 +
         (1.0 - f0) * fresnel) *
        specularLobe;

    const float3 diffuse =
        baseColor *
        (1.0 - metallic) *
        (0.08 + 0.92 * nDotL);

    const float rim =
        pow(
            1.0 - nDotV,
            3.0);

    const float3 color =
        diffuse +
        specular *
            (0.35 + 0.65 * nDotL) +
        baseColor *
            rim * 0.08 +
        emission;

    return float4(
        color,
        1.0);
}
)";

constexpr const char* kSurfacePixelShader = R"(
struct Constants
{
    float4 radiiAndAspect;
    float4 cameraAndTanHalfFov;
    float4 forward;
    float4 up;
    float4 baseColorAndRoughness;
    float4 materialParameters;
};
[[vk::push_constant]] Constants g_pc;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct SurfaceOutputs
{
    float4 baseRoughness : SV_Target0;
    float4 normalMetallic : SV_Target1;
    float4 emissionClass : SV_Target2;
};

float EncodeSurfaceMeta(float surfaceClass, float representation)
{
    return surfaceClass + representation / 16.0;
}

SurfaceOutputs main(VSOutput input)
{
    const float2 p = input.uv;
    const float3 radii =
        max(
            g_pc.radiiAndAspect.xyz,
            float3(0.001, 0.001, 0.001));
    const float3 camera =
        g_pc.cameraAndTanHalfFov.xyz;
    const float3 forward =
        normalize(g_pc.forward.xyz);
    const float3 requestedUp =
        normalize(g_pc.up.xyz);
    const float3 right =
        normalize(cross(forward, requestedUp));
    const float3 cameraUp =
        normalize(cross(right, forward));
    const float tanHalfFov =
        max(g_pc.cameraAndTanHalfFov.w, 0.001);

    const float3 ray =
        normalize(
            forward +
            right *
                (p.x *
                 g_pc.radiiAndAspect.w *
                 tanHalfFov) -
            cameraUp *
                (p.y * tanHalfFov));

    const float3 ro = camera / radii;
    const float3 rd = ray / radii;
    const float a = dot(rd, rd);
    const float b = 2.0 * dot(ro, rd);
    const float c = dot(ro, ro) - 1.0;
    const float discriminant =
        b * b - 4.0 * a * c;

    if (discriminant < 0.0)
        discard;

    const float t =
        (-b - sqrt(discriminant)) /
        (2.0 * a);
    if (t < 0.0)
        discard;

    const float3 hit =
        camera + ray * t;
    const float3 normal =
        normalize(
            float3(
                hit.x / (radii.x * radii.x),
                hit.y / (radii.y * radii.y),
                hit.z / (radii.z * radii.z)));

    SurfaceOutputs output;
    output.baseRoughness =
        float4(
            max(g_pc.baseColorAndRoughness.xyz, 0.0),
            saturate(g_pc.baseColorAndRoughness.w));
    output.normalMetallic =
        float4(
            normal,
            saturate(g_pc.materialParameters.x));
    output.emissionClass =
        float4(
            max(g_pc.materialParameters.yzw, 0.0),
            EncodeSurfaceMeta(
                3.0, // SurfaceClass::RigidGeometry
                6.0)); // SurfaceRepresentation::LocalMesh
    return output;
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

        const shader::Binary surfacePixel =
            compiler.Compile({
                .source = kSurfacePixelShader,
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
                .pushConstantDwords = 24,
                .sampledTextures = 0,
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

        surfacePipeline =
            device.CreateGraphicsPipeline({
                .vertexShader = {
                    .data = vertex.bytecode.data(),
                    .size = vertex.bytecode.size()
                },
                .pixelShader = {
                    .data = surfacePixel.bytecode.data(),
                    .size = surfacePixel.bytecode.size()
                },
                .vertexAttributes = {},
                .vertexStrideBytes = 0,
                .pushConstantDwords = 24,
                .sampledTextures = 0,
                .topology =
                    rhi::PrimitiveTopology::TriangleList,
                .fillMode =
                    rhi::FillMode::Solid,
                .cullMode =
                    rhi::CullMode::None,
                .blendMode =
                    rhi::BlendMode::Opaque,
                .depthTest = false,
                .depthWrite = false,
                .colorAttachmentFormats = {
                    rhi::TextureFormat::RGBA16_Float,
                    rhi::TextureFormat::RGBA16_Float,
                    rhi::TextureFormat::RGBA16_Float
                },
                .colorAttachmentCount = 3U
            });

    }

    std::unique_ptr<rhi::GraphicsPipeline>
        pipeline;
    std::unique_ptr<rhi::GraphicsPipeline>
        surfacePipeline;
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
    const universe::BodyShape& shape,
    const render_view::CameraState& camera,
    const PreviewMaterial& material)
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

    const f64 scale =
        std::max(maximum, 1.0);

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

    const std::array<u32, 24> constants{
        bits(
            static_cast<f32>(
                ellipsoid.radiiMeters.x /
                scale)),
        bits(
            static_cast<f32>(
                ellipsoid.radiiMeters.y /
                scale)),
        bits(
            static_cast<f32>(
                ellipsoid.radiiMeters.z /
                scale)),
        bits(
            static_cast<f32>(width) /
            static_cast<f32>(height)),

        bits(
            static_cast<f32>(
                camera.localPositionMeters.x /
                scale)),
        bits(
            static_cast<f32>(
                camera.localPositionMeters.y /
                scale)),
        bits(
            static_cast<f32>(
                camera.localPositionMeters.z /
                scale)),
        bits(tanHalfFov),

        bits(camera.forward.x),
        bits(camera.forward.y),
        bits(camera.forward.z),
        bits(0.0F),

        bits(camera.up.x),
        bits(camera.up.y),
        bits(camera.up.z),
        bits(0.0F),

        bits(material.baseColor.x),
        bits(material.baseColor.y),
        bits(material.baseColor.z),
        bits(std::clamp(
            material.roughness,
            0.0F,
            1.0F)),

        bits(std::clamp(
            material.metallic,
            0.0F,
            1.0F)),
        bits(std::max(material.emissionRadiance.x, 0.0F)),
        bits(std::max(material.emissionRadiance.y, 0.0F)),
        bits(std::max(material.emissionRadiance.z, 0.0F))
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

void BodyPreviewRenderer::DrawSurfaceData(
    rhi::CommandList& commands,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    const u32 width,
    const u32 height,
    const universe::BodyShape& shape,
    const render_view::CameraState& camera,
    const PreviewMaterial& material)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    const universe::EllipsoidShape ellipsoid =
        std::visit(
            [](const auto& value)
            {
                using Shape =
                    std::decay_t<decltype(value)>;
                if constexpr (
                    std::is_same_v<
                        Shape,
                        universe::SphereShape>)
                {
                    return universe::EllipsoidShape{
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

    const f64 scale =
        std::max(
            std::max({
                ellipsoid.radiiMeters.x,
                ellipsoid.radiiMeters.y,
                ellipsoid.radiiMeters.z
            }),
            1.0);

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const std::array<u32, 24> constants{
        bits(static_cast<f32>(
            ellipsoid.radiiMeters.x / scale)),
        bits(static_cast<f32>(
            ellipsoid.radiiMeters.y / scale)),
        bits(static_cast<f32>(
            ellipsoid.radiiMeters.z / scale)),
        bits(static_cast<f32>(width) /
             static_cast<f32>(height)),

        bits(static_cast<f32>(
            camera.localPositionMeters.x / scale)),
        bits(static_cast<f32>(
            camera.localPositionMeters.y / scale)),
        bits(static_cast<f32>(
            camera.localPositionMeters.z / scale)),
        bits(std::tan(
            camera.verticalFovRadians * 0.5F)),

        bits(camera.forward.x),
        bits(camera.forward.y),
        bits(camera.forward.z),
        0U,

        bits(camera.up.x),
        bits(camera.up.y),
        bits(camera.up.z),
        0U,

        bits(material.baseColor.x),
        bits(material.baseColor.y),
        bits(material.baseColor.z),
        bits(std::clamp(
            material.roughness,
            0.0F,
            1.0F)),

        bits(std::clamp(
            material.metallic,
            0.0F,
            1.0F)),
        0U,
        0U,
        0U
    };

    std::array<rhi::Texture*, 3> targets{
        &surfaceBaseRoughness,
        &surfaceNormalMetallic,
        &surfaceEmissionClass
    };

    commands.SetRenderTargets(
        targets,
        nullptr);
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
        *impl_->surfacePipeline);
    commands.SetGraphicsConstants(
        constants);
    commands.Draw(6);
}
} // namespace orbit::editor_ui
