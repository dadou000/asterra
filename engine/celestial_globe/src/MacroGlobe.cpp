#include <orbit/celestial_globe/MacroGlobe.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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

                result.indices.insert(
                    result.indices.end(),
                    {i0, i2, i1,
                     i1, i2, i3});
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
    const MacroGlobeMesh& mesh)
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

    std::vector<GpuMacroGlobeVertex>
        packed;
    packed.reserve(
        mesh.vertices.size());

    for (const auto& vertex :
         mesh.vertices)
    {
        packed.push_back({
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
        });
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
};

struct Constants
{
    float4 cameraAndAspect;
    float4 forwardAndTanHalfFov;
    float4 upAndScale;
};

[[vk::push_constant]] Constants g_pc;

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
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
    output.normal = normalize(input.normal);
    return output;
}
)";

constexpr const char* kMacroGlobePixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
};

float4 main(VSOutput input) : SV_Target0
{
    const float3 n = normalize(input.normal);
    const float3 l = normalize(float3(0.55, 0.72, -0.48));
    const float ndl = saturate(dot(n, l));
    const float rim = pow(1.0 - saturate(abs(n.z)), 3.0);

    const float3 base = float3(0.12, 0.31, 0.39);
    const float3 color =
        base * (0.055 + 0.945 * ndl) +
        float3(0.07, 0.11, 0.14) * rim;

    return float4(color / (1.0 + color), 1.0);
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

    static constexpr std::array<rhi::VertexAttribute, 2> attributes{{
        {
            .location = 0,
            .format = rhi::VertexFormat::Float3,
            .offsetBytes = 0
        },
        {
            .location = 1,
            .format = rhi::VertexFormat::Float3,
            .offsetBytes = sizeof(math::Float3)
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
        .pushConstantDwords = 12,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::Back,
        .depthTest = false,
        .depthWrite = false
    });
}

void MacroGlobeRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    GpuMacroGlobeProduct& globe,
    const render_view::CameraState& camera)
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

    const std::array<u32, 12> constants{
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
        bits(1.0F)
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
} // namespace orbit::celestial_globe
