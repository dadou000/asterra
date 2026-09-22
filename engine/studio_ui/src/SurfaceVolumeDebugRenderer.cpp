#include <orbit/studio_ui/SurfaceVolumeDebugRenderer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace orbit::studio_ui
{
namespace
{
constexpr const char* kVertexShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_field : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_residency : register(t1);

struct Push
{
    float4 projection;
    float4 forward;
    float4 up;
    float4 camera;
    float4 cellSizeAndScale;
    uint4 layout;
    int4 sliceMeta;
};

[[vk::push_constant]]
Push g;

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

float4 Project(float3 relative)
{
    const float3 forward =
        normalize(g.forward.xyz);
    const float3 requestedUp =
        normalize(g.up.xyz);
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

    const float z =
        dot(relative, forward);

    if (z <= g.projection.z ||
        z >= g.projection.w)
    {
        return float4(
            2.0, 2.0, 1.0, 1.0);
    }

    const float x =
        dot(relative, right);
    const float y =
        dot(relative, cameraUp);

    return float4(
        x /
            (max(g.projection.x, 0.001) *
             max(g.projection.y, 0.001)),
        -y /
            max(g.projection.y, 0.001),
        z * 0.5,
        z);
}

VSOutput main(uint vertexId : SV_VertexID)
{
    VSOutput output;

    const uint tileEdge =
        max(g.layout.x, 1u);
    const uint residentTiles =
        g.layout.y;
    const uint debugSlice =
        g.layout.z;
    const bool vectorField =
        g.layout.w != 0u;

    const uint lineIndex =
        vertexId / 2u;
    const uint endpoint =
        vertexId & 1u;
    const uint cellsPerTileSlice =
        tileEdge * tileEdge;
    const uint slot =
        lineIndex /
        cellsPerTileSlice;

    if (slot >= residentTiles)
    {
        output.position =
            float4(2.0,2.0,1.0,1.0);
        output.color = 0.0;
        return output;
    }

    const uint sliceIndex =
        lineIndex -
        slot *
            cellsPerTileSlice;
    const uint u =
        sliceIndex %
        tileEdge;
    const uint v =
        sliceIndex /
        tileEdge;

    const uint residencyBase =
        slot * 32u;

    const int3 tile =
        int3(
            asint(g_residency.Load(
                residencyBase + 0u)),
            asint(g_residency.Load(
                residencyBase + 4u)),
            asint(g_residency.Load(
                residencyBase + 8u)));

    if (g_residency.Load(
            residencyBase + 16u) == 0u)
    {
        output.position =
            float4(2.0,2.0,1.0,1.0);
        output.color = 0.0;
        return output;
    }

    const uint axis =
        uint(g.sliceMeta.w);
    const uint tileOffset =
        debugSlice /
        tileEdge;
    const uint localSlice =
        debugSlice %
        tileEdge;

    const int3 minimumTile =
        g.sliceMeta.xyz;

    const int targetTile =
        axis == 0u
            ? minimumTile.x +
                int(tileOffset)
            : axis == 1u
                ? minimumTile.y +
                    int(tileOffset)
                : minimumTile.z +
                    int(tileOffset);

    const int actualTile =
        axis == 0u
            ? tile.x
            : axis == 1u
                ? tile.y
                : tile.z;

    if (actualTile != targetTile)
    {
        output.position =
            float4(2.0,2.0,1.0,1.0);
        output.color = 0.0;
        return output;
    }

    uint3 local;

    if (axis == 0u)
    {
        local = uint3(
            localSlice,
            u,
            v);
    }
    else if (axis == 1u)
    {
        local = uint3(
            u,
            localSlice,
            v);
    }
    else
    {
        local = uint3(
            u,
            v,
            localSlice);
    }

    const uint localIndex =
        local.z *
            tileEdge *
            tileEdge +
        local.y *
            tileEdge +
        local.x;
    const uint tileCellCount =
        tileEdge *
        tileEdge *
        tileEdge;
    const uint physicalIndex =
        slot *
            tileCellCount +
        localIndex;

    const float3 cellSize =
        g.cellSizeAndScale.xyz;
    const float scale =
        g.cellSizeAndScale.w;

    const float3 world =
        (float3(tile) *
             float(tileEdge) +
         float3(local) +
         0.5) *
        cellSize;

    float3 endWorld =
        world;
    float4 color;

    if (vectorField)
    {
        const uint address =
            physicalIndex *
            16u;

        const float3 value =
            float3(
                asfloat(g_field.Load(
                    address + 0u)),
                asfloat(g_field.Load(
                    address + 4u)),
                asfloat(g_field.Load(
                    address + 8u)));

        endWorld += value * scale;
        color =
            float4(
                1.0,
                0.62,
                0.12,
                0.95);
    }
    else
    {
        const float value =
            max(
                asfloat(
                    g_field.Load(
                        physicalIndex *
                            4u)),
                0.0);

        const float magnitude =
            min(value, 8.0) *
            scale;

        if (axis == 0u)
            endWorld.x += magnitude;
        else if (axis == 1u)
            endWorld.y += magnitude;
        else
            endWorld.z += magnitude;

        color =
            float4(
                0.18,
                0.84,
                1.0,
                saturate(
                    0.28 +
                    value * 0.22));
    }

    const float3 point =
        endpoint == 0u
            ? world
            : endWorld;

    output.position =
        Project(
            point -
            g.camera.xyz);
    output.color =
        color;

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
} // namespace

SurfaceVolumeDebugRenderer::SurfaceVolumeDebugRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vertex =
        compiler.Compile({
            .source = kVertexShader,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });

    const auto pixel =
        compiler.Compile({
            .source = kPixelShader,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    if (vertex.bytecode.empty() ||
        pixel.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile M34 field debug shaders.");
    }

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
            .vertexStrideBytes = 0U,
            .pushConstantDwords = 28U,
            .shaderResourceBuffers = 2U,
            .sampledTextures = 0U,
            .topology =
                rhi::PrimitiveTopology::
                    LineList,
            .fillMode =
                rhi::FillMode::Solid,
            .cullMode =
                rhi::CullMode::None,
            .blendMode =
                rhi::BlendMode::Alpha,
            .depthTest = false,
            .depthWrite = false,
            .colorAttachmentFormats = {
                rhi::TextureFormat::
                    RGBA16_Float
            },
            .colorAttachmentCount = 1U
        });
}

void SurfaceVolumeDebugRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const render_view::CameraState& camera,
    const world_model::ResolvedVolumeDomain& domain,
    const volume_fields::VolumeFieldDiagnostics& fields,
    const volume_solver::SurfaceVolumeDebugView view,
    const volume_solver::VolumeSliceAxis sliceAxis,
    const world_model::VolumeField fieldChannel,
    const u32 debugLayer,
    rhi::Buffer& field,
    rhi::Buffer& residency)
{
    if (view ==
            volume_solver::
                SurfaceVolumeDebugView::Off ||
        width == 0U ||
        height == 0U ||
        fields.residentTiles == 0U ||
        fields.tileEdge == 0U)
    {
        return;
    }

    const f32 tanHalfFov =
        std::tan(
            camera.verticalFovRadians *
            0.5F);

    const f32 cellX =
        static_cast<f32>(
            domain.halfExtentsMeters.x *
            2.0 /
            std::max<f64>(
                fields.resolutionX,
                1U));
    const f32 cellY =
        static_cast<f32>(
            domain.halfExtentsMeters.y *
            2.0 /
            std::max<f64>(
                fields.resolutionY,
                1U));
    const f32 cellZ =
        static_cast<f32>(
            domain.halfExtentsMeters.z *
            2.0 /
            std::max<f64>(
                fields.resolutionZ,
                1U));

    const math::Double3 tileSize{
        static_cast<f64>(cellX) *
            fields.tileEdge,
        static_cast<f64>(cellY) *
            fields.tileEdge,
        static_cast<f64>(cellZ) *
            fields.tileEdge
    };

    const math::Double3 minimum{
        domain.centerMeters.x -
            domain.halfExtentsMeters.x,
        domain.centerMeters.y -
            domain.halfExtentsMeters.y,
        domain.centerMeters.z -
            domain.halfExtentsMeters.z
    };

    const std::array<i32,3> minimumTile{
        static_cast<i32>(
            std::floor(
                minimum.x /
                tileSize.x)),
        static_cast<i32>(
            std::floor(
                minimum.y /
                tileSize.y)),
        static_cast<i32>(
            std::floor(
                minimum.z /
                tileSize.z))
    };

    const u32 axis =
        static_cast<u32>(
            sliceAxis);

    const std::array<u32,3> resolution{
        fields.resolutionX,
        fields.resolutionY,
        fields.resolutionZ
    };

    const u32 clampedSlice =
        std::min(
            debugLayer,
            std::max(
                resolution[axis],
                1U) -
                1U);

    const auto bits =
        [](const f32 value)
        {
            return
                std::bit_cast<u32>(
                    value);
        };

    std::array<u32,28> constants{};

    constants[0] =
        bits(
            static_cast<f32>(width) /
            static_cast<f32>(height));
    constants[1] =
        bits(tanHalfFov);
    constants[2] =
        bits(camera.nearPlaneMeters);
    constants[3] =
        bits(camera.farPlaneMeters);

    constants[4] = bits(camera.forward.x);
    constants[5] = bits(camera.forward.y);
    constants[6] = bits(camera.forward.z);

    constants[8] = bits(camera.up.x);
    constants[9] = bits(camera.up.y);
    constants[10] = bits(camera.up.z);

    constants[12] =
        bits(
            static_cast<f32>(
                camera.localPositionMeters.x));
    constants[13] =
        bits(
            static_cast<f32>(
                camera.localPositionMeters.y));
    constants[14] =
        bits(
            static_cast<f32>(
                camera.localPositionMeters.z));

    constants[16] = bits(cellX);
    constants[17] = bits(cellY);
    constants[18] = bits(cellZ);
    constants[19] =
        bits(
            volume_fields::FieldKind(
                fieldChannel) ==
                    volume_fields::
                        FieldValueKind::Vector3
                ? 0.35F
                : std::max(
                      std::min({
                          cellX,
                          cellY,
                          cellZ}),
                      0.20F));

    constants[20] =
        fields.tileEdge;
    constants[21] =
        fields.residentTiles;
    constants[22] =
        clampedSlice;
    constants[23] =
        volume_fields::FieldKind(
            fieldChannel) ==
                volume_fields::
                    FieldValueKind::Vector3
            ? 1U
            : 0U;

    constants[24] =
        std::bit_cast<u32>(
            minimumTile[0]);
    constants[25] =
        std::bit_cast<u32>(
            minimumTile[1]);
    constants[26] =
        std::bit_cast<u32>(
            minimumTile[2]);
    constants[27] =
        axis;

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
        *pipeline_);
    commands.SetGraphicsConstants(
        constants);
    commands.SetGraphicsBuffer(
        0U,
        field);
    commands.SetGraphicsBuffer(
        1U,
        residency);

    const u32 lineCount =
        fields.residentTiles *
        fields.tileEdge *
        fields.tileEdge;

    commands.Draw(
        lineCount * 2U);
}
} // namespace orbit::studio_ui
