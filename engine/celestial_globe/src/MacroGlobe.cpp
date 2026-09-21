#include <orbit/celestial_globe/MacroGlobe.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace orbit::celestial_globe
{
namespace
{
[[nodiscard]] f64 ReferenceRadius(
    const universe::BodyShape& shape)
{
    return std::visit(
        [](const auto& value) -> f64
        {
            using Shape =
                std::decay_t<decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    universe::SphereShape>)
            {
                return value.radiusMeters;
            }
            else
            {
                return std::max({
                    value.radiiMeters.x,
                    value.radiiMeters.y,
                    value.radiiMeters.z
                });
            }
        },
        shape);
}

[[nodiscard]] f64 RadiusAlong(
    const universe::BodyShape& shape,
    const math::Double3 direction)
{
    return std::visit(
        [&direction](const auto& value) -> f64
        {
            using Shape =
                std::decay_t<decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    universe::SphereShape>)
            {
                return value.radiusMeters;
            }
            else
            {
                const f64 x =
                    direction.x /
                    value.radiiMeters.x;
                const f64 y =
                    direction.y /
                    value.radiiMeters.y;
                const f64 z =
                    direction.z /
                    value.radiiMeters.z;

                return 1.0 /
                    std::sqrt(
                        x*x + y*y + z*z);
            }
        },
        shape);
}

[[nodiscard]] math::Double3 FaceDirection(
    const u32 face,
    const f64 u,
    const f64 v)
{
    math::Double3 p{};

    switch (face)
    {
    case 0: p = { 1.0, v, -u}; break;
    case 1: p = {-1.0, v,  u}; break;
    case 2: p = { u, 1.0, -v}; break;
    case 3: p = { u,-1.0,  v}; break;
    case 4: p = { u, v, 1.0}; break;
    default:p = {-u, v,-1.0}; break;
    }

    return math::Normalize(p);
}

[[nodiscard]] u64 BuildFingerprint(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const MacroGlobeConfig& config)
{
    u64 value =
        0x4D3135474C4F4245ULL;
    value =
        terrain::StableCombine64(
            value,
            source.Revision());
    value =
        terrain::StableCombine64(
            value,
            config.faceResolution);
    value =
        terrain::StableCombine64(
            value,
            std::bit_cast<u64>(
                config.footprintScale));

    std::visit(
        [&value](const auto& bodyShape)
        {
            if constexpr (
                std::is_same_v<
                    std::decay_t<decltype(bodyShape)>,
                    universe::SphereShape>)
            {
                value =
                    terrain::StableCombine64(
                        value,
                        std::bit_cast<u64>(
                            bodyShape.radiusMeters));
            }
            else
            {
                value =
                    terrain::StableCombine64(
                        value,
                        std::bit_cast<u64>(
                            bodyShape.radiiMeters.x));
                value =
                    terrain::StableCombine64(
                        value,
                        std::bit_cast<u64>(
                            bodyShape.radiiMeters.y));
                value =
                    terrain::StableCombine64(
                        value,
                        std::bit_cast<u64>(
                            bodyShape.radiiMeters.z));
            }
        },
        shape);

    return value;
}
} // namespace

u64 MacroGlobeFingerprint(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const MacroGlobeConfig& config)
{
    if (config.faceResolution < 3U ||
        !std::isfinite(config.footprintScale) ||
        config.footprintScale <= 0.0)
    {
        throw std::invalid_argument(
            "Macro globe config is invalid.");
    }

    return BuildFingerprint(
        source,
        shape,
        config);
}

MacroGlobeMesh BuildMacroGlobe(
    const terrain::TerrainSource& source,
    const universe::BodyShape& shape,
    const MacroGlobeConfig& config)
{
    if (config.faceResolution < 3U ||
        !std::isfinite(config.footprintScale) ||
        config.footprintScale <= 0.0)
    {
        throw std::invalid_argument(
            "Macro globe config is invalid.");
    }

    const f64 referenceRadius =
        ReferenceRadius(shape);

    if (!std::isfinite(referenceRadius) ||
        referenceRadius <= 0.0)
    {
        throw std::invalid_argument(
            "Macro globe body radius is invalid.");
    }

    MacroGlobeMesh result;
    result.referenceRadiusMeters =
        referenceRadius;
    result.sourceRevision =
        source.Revision();
    result.fingerprint =
        BuildFingerprint(
            source,
            shape,
            config);

    const u32 n =
        config.faceResolution;
    const std::size_t verticesPerFace =
        static_cast<std::size_t>(n) * n;

    result.vertices.resize(
        verticesPerFace * 6U);

    result.indices.reserve(
        static_cast<std::size_t>(
            6U * (n - 1U) * (n - 1U) * 6U));

    const f64 angularCell =
        (0.5 * std::numbers::pi_v<f64>) /
        static_cast<f64>(n - 1U);

    result.sampleFootprintMeters =
        referenceRadius *
        angularCell *
        config.footprintScale;

    result.minimumRadiusMeters =
        std::numeric_limits<f64>::max();
    result.maximumRadiusMeters = 0.0;

    for (u32 face = 0; face < 6U; ++face)
    {
        const u32 base =
            face * n * n;

        for (u32 y = 0; y < n; ++y)
        {
            const f64 v =
                -1.0 +
                2.0 *
                static_cast<f64>(y) /
                static_cast<f64>(n - 1U);

            for (u32 x = 0; x < n; ++x)
            {
                const f64 u =
                    -1.0 +
                    2.0 *
                    static_cast<f64>(x) /
                    static_cast<f64>(n - 1U);

                const math::Double3 direction =
                    FaceDirection(
                        face,
                        u,
                        v);

                const auto sample =
                    source.Sample({
                        .unitDirection =
                            direction,
                        .footprintMeters =
                            result.
                                sampleFootprintMeters
                    });

                const f64 radius =
                    RadiusAlong(
                        shape,
                        direction) +
                    sample.elevationMeters;

                const u32 index =
                    base + y * n + x;

                result.vertices[index] = {
                    .positionMeters =
                        direction * radius,
                    .normal = {},
                    .elevationMeters =
                        sample.elevationMeters
                };

                result.minimumRadiusMeters =
                    std::min(
                        result.minimumRadiusMeters,
                        radius);
                result.maximumRadiusMeters =
                    std::max(
                        result.maximumRadiusMeters,
                        radius);
            }
        }

        for (u32 y = 0; y + 1U < n; ++y)
        {
            for (u32 x = 0; x + 1U < n; ++x)
            {
                const u32 i0 =
                    base + y * n + x;
                const u32 i1 =
                    i0 + 1U;
                const u32 i2 =
                    i0 + n;
                const u32 i3 =
                    i2 + 1U;

                const math::Double3 p0 =
                    result.vertices[i0].
                        positionMeters;
                const math::Double3 p1 =
                    result.vertices[i1].
                        positionMeters;
                const math::Double3 p2 =
                    result.vertices[i2].
                        positionMeters;

                const bool outward =
                    math::Dot(
                        math::Cross(
                            p2 - p0,
                            p1 - p0),
                        p0) > 0.0;

                if (outward)
                {
                    result.indices.insert(
                        result.indices.end(),
                        {i0, i2, i1,
                         i1, i2, i3});
                }
                else
                {
                    result.indices.insert(
                        result.indices.end(),
                        {i0, i1, i2,
                         i1, i3, i2});
                }
            }
        }
    }

    const f64 epsilon =
        std::max(
            angularCell * 0.35,
            1.0e-6);

    const auto displacedPosition =
        [&](const math::Double3 direction)
        {
            const math::Double3 unit =
                math::Normalize(direction);

            const auto sample =
                source.Sample({
                    .unitDirection = unit,
                    .footprintMeters =
                        result.sampleFootprintMeters
                });

            return unit *
                (RadiusAlong(
                     shape,
                     unit) +
                 sample.elevationMeters);
        };

    for (auto& vertex : result.vertices)
    {
        const math::Double3 direction =
            math::Normalize(
                vertex.positionMeters);

        const math::Double3 reference =
            std::abs(direction.y) < 0.9
                ? math::Double3{0.0, 1.0, 0.0}
                : math::Double3{1.0, 0.0, 0.0};

        const math::Double3 tangentA =
            math::Normalize(
                math::Cross(
                    reference,
                    direction));
        const math::Double3 tangentB =
            math::Normalize(
                math::Cross(
                    direction,
                    tangentA));

        const math::Double3 aMinus =
            displacedPosition(
                direction -
                tangentA * epsilon);
        const math::Double3 aPlus =
            displacedPosition(
                direction +
                tangentA * epsilon);
        const math::Double3 bMinus =
            displacedPosition(
                direction -
                tangentB * epsilon);
        const math::Double3 bPlus =
            displacedPosition(
                direction +
                tangentB * epsilon);

        math::Double3 normal =
            math::Cross(
                aPlus - aMinus,
                bPlus - bMinus);

        if (math::Dot(
                normal,
                direction) < 0.0)
        {
            normal =
                normal * -1.0;
        }

        vertex.normal =
            math::LengthSquared(normal) >
                    1.0e-24
                ? math::Normalize(normal)
                : direction;
    }

    return result;
}

GpuMacroGlobeProduct::GpuMacroGlobeProduct(
    rhi::Device& device,
    const MacroGlobeMesh& mesh,
    const celestial_appearance::PlanetaryAppearanceProduct*
        appearance)
    : indexCount_(
          static_cast<u32>(
              mesh.indices.size())),
      referenceRadiusMeters_(
          mesh.referenceRadiusMeters),
      fingerprint_(
          mesh.fingerprint)
{
    if (mesh.vertices.empty() ||
        mesh.indices.empty() ||
        mesh.referenceRadiusMeters <= 0.0)
    {
        throw std::invalid_argument(
            "GPU macro globe requires a valid CPU mesh.");
    }

    if (appearance != nullptr &&
        (appearance->texels.size() !=
             mesh.vertices.size() ||
         appearance->faceResolution *
             appearance->faceResolution *
             6U !=
             appearance->texels.size()))
    {
        throw std::invalid_argument(
            "Planetary appearance topology does not match macro globe topology.");
    }

    std::vector<GpuMacroGlobeVertex>
        packed;
    packed.reserve(
        mesh.vertices.size());

    for (std::size_t index = 0;
         index < mesh.vertices.size();
         ++index)
    {
        const auto& vertex =
            mesh.vertices[index];

        GpuMacroGlobeVertex packedVertex{
            .positionNormalized = {
                static_cast<f32>(
                    vertex.positionMeters.x /
                    mesh.referenceRadiusMeters),
                static_cast<f32>(
                    vertex.positionMeters.y /
                    mesh.referenceRadiusMeters),
                static_cast<f32>(
                    vertex.positionMeters.z /
                    mesh.referenceRadiusMeters)
            },
            .normal = {
                static_cast<f32>(
                    vertex.normal.x),
                static_cast<f32>(
                    vertex.normal.y),
                static_cast<f32>(
                    vertex.normal.z)
            }
        };

        if (appearance != nullptr)
        {
            const auto& texel =
                appearance->texels[index];

            packedVertex.albedoLinear =
                texel.albedoLinear;
            packedVertex.appearanceNormal =
                texel.normal;
            packedVertex.materialChannels = {
                texel.roughness,
                texel.oceanMask,
                texel.iceMask,
                texel.directLightTransmittance
            };
            packedVertex.emissionLinear =
                texel.emissionLinear;
        }
        else
        {
            packedVertex.appearanceNormal =
                packedVertex.normal;
        }

        packed.push_back(
            packedVertex);
    }

    vertices_ =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    packed.size() *
                    sizeof(
                        GpuMacroGlobeVertex)),
            .usage =
                rhi::BufferUsage::Vertex,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::
                    VertexOrConstantBuffer
        });

    indices_ =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    mesh.indices.size() *
                    sizeof(u32)),
            .usage =
                rhi::BufferUsage::Index,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::
                    IndexBuffer
        });

    if (!vertices_ || !indices_)
    {
        throw std::runtime_error(
            "Failed to allocate macro globe GPU buffers.");
    }

    std::memcpy(
        vertices_->Map(),
        packed.data(),
        packed.size() *
            sizeof(GpuMacroGlobeVertex));
    vertices_->Unmap();

    std::memcpy(
        indices_->Map(),
        mesh.indices.data(),
        mesh.indices.size() *
            sizeof(u32));
    indices_->Unmap();
}

rhi::Buffer&
GpuMacroGlobeProduct::VertexBuffer() noexcept
{
    return *vertices_;
}

rhi::Buffer&
GpuMacroGlobeProduct::IndexBuffer() noexcept
{
    return *indices_;
}

u32 GpuMacroGlobeProduct::IndexCount() const noexcept
{
    return indexCount_;
}

f64 GpuMacroGlobeProduct::
ReferenceRadiusMeters() const noexcept
{
    return referenceRadiusMeters_;
}

u64 GpuMacroGlobeProduct::Fingerprint() const noexcept
{
    return fingerprint_;
}
} // namespace orbit::celestial_globe


namespace orbit::celestial_globe
{
namespace
{
constexpr const char* kMacroGlobeVertexShader = R"(
struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 albedo : COLOR0;
    float3 appearanceNormal : NORMAL1;
    float4 material : TEXCOORD0;
    float3 emission : COLOR1;
};

struct Constants
{
    float4 cameraAndAspect;
    float4 forwardAndTanHalfFov;
    float4 upAndScale;
    float4 transition;
    float4 lighting;
    float4 ocean;
};

[[vk::push_constant]] Constants g_pc;

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 albedo : COLOR0;
    float4 material : TEXCOORD0;
    float3 emission : COLOR1;
    float opacity : TEXCOORD1;
    float3 lightDirection : TEXCOORD2;
    float lightScale : TEXCOORD3;
    float3 viewDirection : TEXCOORD4;
};

VSOutput main(VSInput input)
{
    const float3 forward = normalize(g_pc.forwardAndTanHalfFov.xyz);
    const float3 upRequested = normalize(g_pc.upAndScale.xyz);
    const float3 right = normalize(cross(forward, upRequested));
    const float3 up = normalize(cross(right, forward));

    const float3 camera = g_pc.cameraAndAspect.xyz;
    const float3 world = input.position * g_pc.upAndScale.w;
    const float3 relative = world - camera;

    const float z = dot(relative, forward);
    const float x = dot(relative, right);
    const float y = dot(relative, up);

    const float tanHalf = max(g_pc.forwardAndTanHalfFov.w, 0.001);
    const float aspect = max(g_pc.cameraAndAspect.w, 0.001);

    VSOutput output;
    output.position = float4(
        x / (tanHalf * aspect),
        y / tanHalf,
        z * 0.5,
        z);
    output.normal = normalize(input.appearanceNormal);
    output.albedo = input.albedo;
    output.material = input.material;
    output.emission = input.emission;
    output.opacity = saturate(g_pc.transition.x);
    output.lightDirection =
        normalize(g_pc.lighting.xyz);
    output.lightScale =
        max(g_pc.lighting.w, 0.0);
    output.viewDirection =
        normalize(camera - world);
    return output;
}
)";

constexpr const char* kMacroGlobePixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 albedo : COLOR0;
    float4 material : TEXCOORD0;
    float3 emission : COLOR1;
    float opacity : TEXCOORD1;
    float3 lightDirection : TEXCOORD2;
    float lightScale : TEXCOORD3;
    float3 viewDirection : TEXCOORD4;
};

float4 main(VSOutput input) : SV_Target0
{
    const float3 n = normalize(input.normal);
    const float3 l =
        normalize(input.lightDirection);
    const float ndl = saturate(dot(n, l));

    const float roughness = saturate(input.material.x);
    const float oceanMask = saturate(input.material.y);
    const float ice = saturate(input.material.z);
    const float cloudTransmission = saturate(input.material.w);

    const float3 v = normalize(input.viewDirection);
    const float3 h = normalize(l + v);
    const float ndv = saturate(dot(n, v));
    const float ndh = saturate(dot(n, h));
    const float vdh = saturate(dot(v, h));

    const float eta = max(g_pc.ocean.x, 1.0);
    const float f0 =
        pow((eta - 1.0) / (eta + 1.0), 2.0);
    const float oceanRoughness =
        clamp(g_pc.ocean.y, 0.01, 1.0);
    const float alpha =
        max(oceanRoughness * oceanRoughness, 0.0001);
    const float a2 = alpha * alpha;
    const float denom =
        ndh * ndh * (a2 - 1.0) + 1.0;
    const float D =
        a2 / max(3.14159265 * denom * denom, 1e-6);
    const float k =
        (oceanRoughness + 1.0) *
        (oceanRoughness + 1.0) / 8.0;
    const float Gl =
        ndl / max(ndl * (1.0 - k) + k, 1e-5);
    const float Gv =
        ndv / max(ndv * (1.0 - k) + k, 1e-5);
    const float F =
        f0 + (1.0 - f0) *
        pow(1.0 - vdh, 5.0);

    const float oceanEnabled =
        saturate(g_pc.ocean.w);
    const float glint =
        oceanEnabled *
        oceanMask *
        (1.0 - ice) *
        max(g_pc.ocean.z, 0.0) *
        D * Gl * Gv * F /
        max(4.0 * ndl * ndv, 1e-5);

    const float direct =
        ndl *
        input.lightScale *
        cloudTransmission;

    const float diffuse =
        0.045 * input.lightScale +
        0.955 * direct;

    float3 color =
        input.albedo * diffuse +
        glint *
            input.lightScale *
            cloudTransmission *
            float3(1.0, 0.98, 0.94) +
        ice * 0.03 +
        input.emission;

    return float4(
        color,
        saturate(input.opacity));
}
)";

constexpr const char* kMacroGlobeSurfacePixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 albedo : COLOR0;
    float4 material : TEXCOORD0;
    float3 emission : COLOR1;
    float opacity : TEXCOORD1;
    float3 lightDirection : TEXCOORD2;
    float lightScale : TEXCOORD3;
    float3 viewDirection : TEXCOORD4;
};

struct SurfaceOutputs
{
    float4 previewColor : SV_Target0;
    float4 baseRoughness : SV_Target1;
    float4 normalMetallic : SV_Target2;
    float4 emissionClass : SV_Target3;
};

float EncodeSurfaceMeta(float surfaceClass, float representation)
{
    return surfaceClass + representation / 16.0;
}

SurfaceOutputs main(VSOutput input)
{
    const float3 n = normalize(input.normal);
    const float3 l = normalize(input.lightDirection);
    const float ndl = saturate(dot(n, l));

    const float roughness = saturate(input.material.x);
    const float ocean = saturate(input.material.y);
    const float ice = saturate(input.material.z);

    const float diffuse =
        (0.045 + 0.955 * ndl) *
        input.lightScale;

    const float grazing =
        pow(1.0 - saturate(ndl), 5.0);

    const float specularStrength =
        lerp(0.08, 0.55, ocean) *
        (1.0 - roughness * 0.75);

    const float3 color =
        input.albedo * diffuse +
        specularStrength * grazing *
            float3(0.45, 0.58, 0.68) +
        ice * 0.03 +
        input.emission;

    SurfaceOutputs output;
    output.previewColor =
        float4(color, saturate(input.opacity));
    output.baseRoughness =
        float4(input.albedo, roughness);
    output.normalMetallic =
        float4(n, 0.0);
    output.emissionClass =
        float4(
            max(input.emission, 0.0),
            EncodeSurfaceMeta(
                6.0, // SurfaceClass::CelestialSurface
                2.0)); // SurfaceRepresentation::MacroGlobe
    return output;
}
)";
} // namespace

MacroGlobeRenderer::MacroGlobeRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vs = compiler.Compile({
        .source = kMacroGlobeVertexShader,
        .entryPoint = "main",
        .stage = shader::Stage::Vertex,
        .debug = false
    });

    const auto ps = compiler.Compile({
        .source = kMacroGlobePixelShader,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false
    });

    const auto surfacePs = compiler.Compile({
        .source = kMacroGlobeSurfacePixelShader,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false
    });

    static constexpr std::array<rhi::VertexAttribute, 6> attributes{{
        {
            .location = 0,
            .format = rhi::VertexFormat::Float3,
            .offsetBytes = static_cast<u32>(
                offsetof(
                    GpuMacroGlobeVertex,
                    positionNormalized))
        },
        {
            .location = 1,
            .format = rhi::VertexFormat::Float3,
            .offsetBytes = static_cast<u32>(
                offsetof(
                    GpuMacroGlobeVertex,
                    normal))
        },
        {
            .location = 2,
            .format = rhi::VertexFormat::Float3,
            .offsetBytes = static_cast<u32>(
                offsetof(
                    GpuMacroGlobeVertex,
                    albedoLinear))
        },
        {
            .location = 3,
            .format = rhi::VertexFormat::Float3,
            .offsetBytes = static_cast<u32>(
                offsetof(
                    GpuMacroGlobeVertex,
                    appearanceNormal))
        },
        {
            .location = 4,
            .format = rhi::VertexFormat::Float4,
            .offsetBytes = static_cast<u32>(
                offsetof(
                    GpuMacroGlobeVertex,
                    materialChannels))
        },
        {
            .location = 5,
            .format = rhi::VertexFormat::Float3,
            .offsetBytes = static_cast<u32>(
                offsetof(
                    GpuMacroGlobeVertex,
                    emissionLinear))
        }
    }};

    pipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {
            .data = vs.bytecode.data(),
            .size = vs.bytecode.size()
        },
        .pixelShader = {
            .data = ps.bytecode.data(),
            .size = ps.bytecode.size()
        },
        .vertexAttributes = attributes,
        .vertexStrideBytes = sizeof(GpuMacroGlobeVertex),
        .pushConstantDwords = 24,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::Back,
        .blendMode = rhi::BlendMode::Alpha,
        .depthTest = false,
        .depthWrite = false,
        .colorAttachmentFormats = {
            rhi::TextureFormat::RGBA16_Float
        },
        .colorAttachmentCount = 1U
    });

    surfacePipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {
            .data = vs.bytecode.data(),
            .size = vs.bytecode.size()
        },
        .pixelShader = {
            .data = surfacePs.bytecode.data(),
            .size = surfacePs.bytecode.size()
        },
        .vertexAttributes = attributes,
        .vertexStrideBytes = sizeof(GpuMacroGlobeVertex),
        .pushConstantDwords = 24,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::Back,
        .blendMode = rhi::BlendMode::Opaque,
        .depthTest = false,
        .depthWrite = false,
        .colorAttachmentFormats = {
            rhi::TextureFormat::RGBA16_Float,
            rhi::TextureFormat::RGBA16_Float,
            rhi::TextureFormat::RGBA16_Float,
            rhi::TextureFormat::RGBA16_Float
        },
        .colorAttachmentCount = 4U
    });
}

void MacroGlobeRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    GpuMacroGlobeProduct& globe,
    const render_view::CameraState& camera,
    const f32 opacity,
    const MacroGlobeLighting& lighting)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    const f64 radius =
        std::max(
            globe.ReferenceRadiusMeters(),
            1.0);

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const std::array<u32, 24> constants{
        bits(static_cast<f32>(camera.localPositionMeters.x / radius)),
        bits(static_cast<f32>(camera.localPositionMeters.y / radius)),
        bits(static_cast<f32>(camera.localPositionMeters.z / radius)),
        bits(static_cast<f32>(width) / static_cast<f32>(height)),

        bits(camera.forward.x),
        bits(camera.forward.y),
        bits(camera.forward.z),
        bits(std::tan(camera.verticalFovRadians * 0.5F)),

        bits(camera.up.x),
        bits(camera.up.y),
        bits(camera.up.z),
        bits(1.0F),

        bits(std::clamp(opacity, 0.0F, 1.0F)),
        0U,
        0U,
        0U,

        bits(lighting.directionBody.x),
        bits(lighting.directionBody.y),
        bits(lighting.directionBody.z),
        bits(std::max(
            lighting.irradianceScale,
            0.0F)),

        bits(std::max(
            lighting.oceanRefractiveIndex,
            1.0F)),
        bits(std::clamp(
            lighting.oceanRoughness,
            0.01F,
            1.0F)),
        bits(std::max(
            lighting.oceanGlintStrength,
            0.0F)),
        bits(lighting.oceanEnabled
            ? 1.0F
            : 0.0F)
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
    commands.SetGraphicsPipeline(*pipeline_);
    commands.SetGraphicsConstants(constants);
    commands.SetVertexBuffer(
        globe.VertexBuffer(),
        sizeof(GpuMacroGlobeVertex));
    commands.SetIndexBuffer(
        globe.IndexBuffer(),
        rhi::IndexFormat::UInt32);
    commands.DrawIndexed(
        globe.IndexCount());
}

void MacroGlobeRenderer::DrawSurface(
    rhi::CommandList& commands,
    rhi::Texture& previewColor,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    const u32 width,
    const u32 height,
    GpuMacroGlobeProduct& globe,
    const render_view::CameraState& camera,
    const f32 opacity,
    const MacroGlobeLighting& lighting)
{
    if (width == 0U || height == 0U)
    {
        return;
    }

    const f64 radius =
        std::max(
            globe.ReferenceRadiusMeters(),
            1.0);

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const std::array<u32, 24> constants{
        bits(static_cast<f32>(camera.localPositionMeters.x / radius)),
        bits(static_cast<f32>(camera.localPositionMeters.y / radius)),
        bits(static_cast<f32>(camera.localPositionMeters.z / radius)),
        bits(static_cast<f32>(width) / static_cast<f32>(height)),

        bits(camera.forward.x),
        bits(camera.forward.y),
        bits(camera.forward.z),
        bits(std::tan(camera.verticalFovRadians * 0.5F)),

        bits(camera.up.x),
        bits(camera.up.y),
        bits(camera.up.z),
        bits(1.0F),

        bits(std::clamp(opacity, 0.0F, 1.0F)),
        0U,
        0U,
        0U,

        bits(lighting.directionBody.x),
        bits(lighting.directionBody.y),
        bits(lighting.directionBody.z),
        bits(std::max(
            lighting.irradianceScale,
            0.0F)),

        bits(std::max(
            lighting.oceanRefractiveIndex,
            1.0F)),
        bits(std::clamp(
            lighting.oceanRoughness,
            0.01F,
            1.0F)),
        bits(std::max(
            lighting.oceanGlintStrength,
            0.0F)),
        bits(lighting.oceanEnabled
            ? 1.0F
            : 0.0F)
    };

    std::array<rhi::Texture*, 4> targets{
        &previewColor,
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
    commands.SetGraphicsPipeline(*surfacePipeline_);
    commands.SetGraphicsConstants(constants);
    commands.SetVertexBuffer(
        globe.VertexBuffer(),
        sizeof(GpuMacroGlobeVertex));
    commands.SetIndexBuffer(
        globe.IndexBuffer(),
        rhi::IndexFormat::UInt32);
    commands.DrawIndexed(
        globe.IndexCount());
}
} // namespace orbit::celestial_globe
