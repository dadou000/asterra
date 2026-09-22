#include <orbit/volume_solver/SurfaceVolumeSolver.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace orbit::volume_solver
{
namespace
{
constexpr u32 kThreadGroupSize = 64U;
constexpr u32 kMaximumInputs = 256U;
constexpr u32 kInvalidSlot = ~0U;

struct TileCoordHash
{
    [[nodiscard]] std::size_t operator()(
        const volume_fields::TileCoord value) const noexcept
    {
        std::size_t seed =
            static_cast<std::size_t>(
                static_cast<u32>(value.x));
        seed ^=
            static_cast<std::size_t>(
                static_cast<u32>(value.y)) +
            0x9e3779b9U +
            (seed << 6U) +
            (seed >> 2U);
        seed ^=
            static_cast<std::size_t>(
                static_cast<u32>(value.z)) +
            0x9e3779b9U +
            (seed << 6U) +
            (seed >> 2U);
        return seed;
    }
};

struct alignas(16) NeighborRecord
{
    std::array<u32, 4> horizontal{
        kInvalidSlot,
        kInvalidSlot,
        kInvalidSlot,
        kInvalidSlot};
    std::array<u32, 4> vertical{
        kInvalidSlot,
        kInvalidSlot,
        0U,
        0U};
};

struct alignas(16) InputRecord
{
    std::array<f32, 4> positionRadius{};
    std::array<f32, 4> vectorScalar{};
    std::array<f32, 4> halfEnabled{};
    std::array<u32, 4> meta{};
};

static_assert(
    sizeof(NeighborRecord) == 32U);
static_assert(
    sizeof(InputRecord) == 64U);

[[nodiscard]] std::unique_ptr<rhi::ComputePipeline>
CompileCompute(
    rhi::Device& device,
    const shader::Compiler& compiler,
    const char* source,
    const u32 pushDwords,
    const u32 buffers)
{
    const auto binary =
        compiler.Compile({
            .source = source,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    if (binary.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile an M33 surface-volume shader.");
    }

    return device.CreateComputePipeline({
        .computeShader = {
            .data = binary.bytecode.data(),
            .size = binary.bytecode.size()
        },
        .pushConstantDwords = pushDwords,
        .shaderResourceBuffers = buffers
    });
}

[[nodiscard]] f32 Decay(
    const f32 value,
    const f32 perSecond,
    const f32 dt) noexcept
{
    return
        value *
        std::exp(
            -std::max(perSecond, 0.0F) *
            std::max(dt, 0.0F));
}

[[nodiscard]] std::size_t CpuIndex(
    const u32 x,
    const u32 y,
    const u32 z,
    const SurfaceVolumeReferenceConfig& config)
{
    return
        (static_cast<std::size_t>(z) *
             config.layers +
         y) *
            config.width +
        x;
}

constexpr const char* kClearShader = R"(
[[vk::binding(0, 0)]]
RWByteAddressBuffer g_output : register(u0);

struct PushConstants
{
    uint dwordCount;
};

[[vk::push_constant]]
PushConstants g_pc;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.dwordCount)
        return;

    g_output.Store(
        id.x * 4u,
        0u);
}
)";

constexpr const char* kVelocityShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_velocityIn : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_residency : register(t1);
[[vk::binding(2, 0)]]
ByteAddressBuffer g_neighbors : register(t2);
[[vk::binding(3, 0)]]
ByteAddressBuffer g_inputs : register(t3);
[[vk::binding(4, 0)]]
RWByteAddressBuffer g_velocityOut : register(u4);

struct PushConstants
{
    uint totalCells;
    uint tileEdge;
    uint tileCellCount;
    uint inputCount;

    float dt;
    float cellX;
    float cellY;
    float cellZ;

    float dissipation;
    float sourceScale;
    uint velocityFieldBit;
    uint padding;
};

[[vk::push_constant]]
PushConstants g_pc;

uint CellAddress(uint slot, uint3 local)
{
    const uint edge = g_pc.tileEdge;
    const uint localIndex =
        (local.z * edge * edge) +
        (local.y * edge) +
        local.x;
    return
        (slot * g_pc.tileCellCount +
         localIndex) *
        16u;
}

float3 LoadVelocity(uint slot, uint3 local)
{
    const uint address =
        CellAddress(slot, local);

    return float3(
        asfloat(g_velocityIn.Load(address + 0u)),
        asfloat(g_velocityIn.Load(address + 4u)),
        asfloat(g_velocityIn.Load(address + 8u)));
}

uint NeighborSlot(uint slot, uint axis)
{
    const uint base = slot * 32u;
    return g_neighbors.Load(
        base + axis * 4u);
}

float3 NeighborVelocity(
    uint slot,
    uint3 local,
    uint axis,
    bool positive)
{
    uint3 q = local;
    const uint edge = g_pc.tileEdge;

    if (axis == 0u)
    {
        if (!positive && q.x > 0u)
        {
            q.x -= 1u;
            return LoadVelocity(slot, q);
        }

        if (positive && q.x + 1u < edge)
        {
            q.x += 1u;
            return LoadVelocity(slot, q);
        }
    }
    else if (axis == 1u)
    {
        if (!positive && q.y > 0u)
        {
            q.y -= 1u;
            return LoadVelocity(slot, q);
        }

        if (positive && q.y + 1u < edge)
        {
            q.y += 1u;
            return LoadVelocity(slot, q);
        }
    }
    else
    {
        if (!positive && q.z > 0u)
        {
            q.z -= 1u;
            return LoadVelocity(slot, q);
        }

        if (positive && q.z + 1u < edge)
        {
            q.z += 1u;
            return LoadVelocity(slot, q);
        }
    }

    const uint direction =
        axis == 0u
            ? (positive ? 1u : 0u)
            : axis == 1u
                ? (positive ? 5u : 4u)
                : (positive ? 3u : 2u);

    const uint neighbor =
        NeighborSlot(slot, direction);

    if (neighbor == 0xffffffffu)
        return LoadVelocity(slot, local);

    if (axis == 0u)
        q.x = positive ? 0u : edge - 1u;
    else if (axis == 1u)
        q.y = positive ? 0u : edge - 1u;
    else
        q.z = positive ? 0u : edge - 1u;

    return LoadVelocity(neighbor, q);
}

float3 WorldPosition(
    uint slot,
    uint3 local)
{
    const uint base =
        slot * 32u;

    const int3 tile =
        int3(
            asint(g_residency.Load(base + 0u)),
            asint(g_residency.Load(base + 4u)),
            asint(g_residency.Load(base + 8u)));

    const float3 cell =
        float3(
            g_pc.cellX,
            g_pc.cellY,
            g_pc.cellZ);

    return
        (float3(tile) *
             float(g_pc.tileEdge) +
         float3(local) +
         0.5) *
        cell;
}

float Influence(
    float3 worldPosition,
    uint inputIndex)
{
    const uint base =
        inputIndex * 64u;

    const float3 center =
        float3(
            asfloat(g_inputs.Load(base + 0u)),
            asfloat(g_inputs.Load(base + 4u)),
            asfloat(g_inputs.Load(base + 8u)));

    const float radius =
        max(
            asfloat(g_inputs.Load(base + 12u)),
            0.0001);

    const float3 halfExtent =
        max(
            float3(
                asfloat(g_inputs.Load(base + 32u)),
                asfloat(g_inputs.Load(base + 36u)),
                asfloat(g_inputs.Load(base + 40u))),
            0.0001);

    const uint shape =
        g_inputs.Load(base + 56u);

    if (shape == 0u ||
        shape == 1u)
    {
        const float distanceToCenter =
            length(
                worldPosition - center);

        return
            saturate(
                1.0 -
                distanceToCenter /
                    radius);
    }

    const float3 normalized =
        abs(
            worldPosition - center) /
        halfExtent;

    return
        saturate(
            1.0 -
            max(
                normalized.x,
                max(
                    normalized.y,
                    normalized.z)));
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.totalCells)
        return;

    const uint slot =
        id.x /
        g_pc.tileCellCount;
    const uint localIndex =
        id.x -
        slot * g_pc.tileCellCount;

    const uint edge =
        g_pc.tileEdge;
    const uint3 local =
        uint3(
            localIndex % edge,
            (localIndex / edge) % edge,
            localIndex /
                (edge * edge));

    float3 velocity =
        LoadVelocity(slot, local);

    const float3 xMinus =
        NeighborVelocity(slot, local, 0u, false);
    const float3 xPlus =
        NeighborVelocity(slot, local, 0u, true);
    const float3 yMinus =
        NeighborVelocity(slot, local, 1u, false);
    const float3 yPlus =
        NeighborVelocity(slot, local, 1u, true);
    const float3 zMinus =
        NeighborVelocity(slot, local, 2u, false);
    const float3 zPlus =
        NeighborVelocity(slot, local, 2u, true);

    const float3 dx =
        velocity.x >= 0.0
            ? (velocity - xMinus) /
                max(g_pc.cellX, 0.0001)
            : (xPlus - velocity) /
                max(g_pc.cellX, 0.0001);

    const float3 dy =
        velocity.y >= 0.0
            ? (velocity - yMinus) /
                max(g_pc.cellY, 0.0001)
            : (yPlus - velocity) /
                max(g_pc.cellY, 0.0001);

    const float3 dz =
        velocity.z >= 0.0
            ? (velocity - zMinus) /
                max(g_pc.cellZ, 0.0001)
            : (zPlus - velocity) /
                max(g_pc.cellZ, 0.0001);

    float3 solved =
        velocity -
        g_pc.dt *
            (velocity.x * dx +
             velocity.y * dy +
             velocity.z * dz);

    solved *=
        exp(
            -max(
                g_pc.dissipation,
                0.0) *
            g_pc.dt);

    const float3 worldPosition =
        WorldPosition(
            slot,
            local);

    [loop]
    for (uint inputIndex = 0u;
         inputIndex < g_pc.inputCount;
         ++inputIndex)
    {
        const uint base =
            inputIndex * 64u;

        const bool enabled =
            asfloat(
                g_inputs.Load(
                    base + 44u)) >
            0.5;

        if (!enabled)
            continue;

        const uint role =
            g_inputs.Load(
                base + 48u);
        const uint kind =
            g_inputs.Load(
                base + 52u);
        const uint fieldMask =
            g_inputs.Load(
                base + 60u);

        if ((fieldMask &
             g_pc.velocityFieldBit) == 0u)
            continue;

        const float weight =
            Influence(
                worldPosition,
                inputIndex);

        if (weight <= 0.0)
            continue;

        const float3 vectorValue =
            float3(
                asfloat(g_inputs.Load(base + 16u)),
                asfloat(g_inputs.Load(base + 20u)),
                asfloat(g_inputs.Load(base + 24u)));

        const float scalarValue =
            asfloat(
                g_inputs.Load(
                    base + 28u));

        if (role == 0u)
        {
            solved +=
                vectorValue *
                (g_pc.sourceScale *
                 g_pc.dt *
                 weight);
        }
        else
        {
            if (kind == 0u)
            {
                solved =
                    lerp(
                        solved,
                        0.0,
                        weight);
            }
            else if (kind == 1u)
            {
                solved *=
                    exp(
                        -abs(scalarValue) *
                        g_pc.dt *
                        weight);
            }
            else if (kind == 2u)
            {
                solved +=
                    vectorValue *
                    (g_pc.dt *
                     weight);
            }
        }
    }

    const uint outAddress =
        CellAddress(
            slot,
            local);

    g_velocityOut.Store(
        outAddress + 0u,
        asuint(solved.x));
    g_velocityOut.Store(
        outAddress + 4u,
        asuint(solved.y));
    g_velocityOut.Store(
        outAddress + 8u,
        asuint(solved.z));
    g_velocityOut.Store(
        outAddress + 12u,
        0u);
}
)";

constexpr const char* kScalarShader = R"(
[[vk::binding(0, 0)]]
ByteAddressBuffer g_scalarIn : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_velocity : register(t1);
[[vk::binding(2, 0)]]
ByteAddressBuffer g_residency : register(t2);
[[vk::binding(3, 0)]]
ByteAddressBuffer g_neighbors : register(t3);
[[vk::binding(4, 0)]]
ByteAddressBuffer g_inputs : register(t4);
[[vk::binding(5, 0)]]
RWByteAddressBuffer g_scalarOut : register(u5);

struct PushConstants
{
    uint totalCells;
    uint tileEdge;
    uint tileCellCount;
    uint inputCount;

    float dt;
    float cellX;
    float cellY;
    float cellZ;

    float dissipation;
    float sourceScale;
    uint fieldBit;
    uint padding;
};

[[vk::push_constant]]
PushConstants g_pc;

uint ScalarAddress(uint slot, uint3 local)
{
    const uint edge = g_pc.tileEdge;
    const uint localIndex =
        local.z * edge * edge +
        local.y * edge +
        local.x;

    return
        (slot * g_pc.tileCellCount +
         localIndex) *
        4u;
}

uint VelocityAddress(uint slot, uint3 local)
{
    const uint edge = g_pc.tileEdge;
    const uint localIndex =
        local.z * edge * edge +
        local.y * edge +
        local.x;

    return
        (slot * g_pc.tileCellCount +
         localIndex) *
        16u;
}

float LoadScalar(uint slot, uint3 local)
{
    return
        asfloat(
            g_scalarIn.Load(
                ScalarAddress(
                    slot,
                    local)));
}

float3 LoadVelocity(uint slot, uint3 local)
{
    const uint address =
        VelocityAddress(
            slot,
            local);

    return float3(
        asfloat(g_velocity.Load(address + 0u)),
        asfloat(g_velocity.Load(address + 4u)),
        asfloat(g_velocity.Load(address + 8u)));
}

uint NeighborSlot(uint slot, uint axis)
{
    return
        g_neighbors.Load(
            slot * 32u +
            axis * 4u);
}

float NeighborScalar(
    uint slot,
    uint3 local,
    uint axis,
    bool positive)
{
    uint3 q = local;
    const uint edge =
        g_pc.tileEdge;

    if (axis == 0u)
    {
        if (!positive && q.x > 0u)
        {
            q.x -= 1u;
            return LoadScalar(slot, q);
        }

        if (positive && q.x + 1u < edge)
        {
            q.x += 1u;
            return LoadScalar(slot, q);
        }
    }
    else if (axis == 1u)
    {
        if (!positive && q.y > 0u)
        {
            q.y -= 1u;
            return LoadScalar(slot, q);
        }

        if (positive && q.y + 1u < edge)
        {
            q.y += 1u;
            return LoadScalar(slot, q);
        }
    }
    else
    {
        if (!positive && q.z > 0u)
        {
            q.z -= 1u;
            return LoadScalar(slot, q);
        }

        if (positive && q.z + 1u < edge)
        {
            q.z += 1u;
            return LoadScalar(slot, q);
        }
    }

    const uint direction =
        axis == 0u
            ? (positive ? 1u : 0u)
            : axis == 1u
                ? (positive ? 5u : 4u)
                : (positive ? 3u : 2u);

    const uint neighbor =
        NeighborSlot(
            slot,
            direction);

    if (neighbor == 0xffffffffu)
        return LoadScalar(slot, local);

    if (axis == 0u)
        q.x = positive ? 0u : edge - 1u;
    else if (axis == 1u)
        q.y = positive ? 0u : edge - 1u;
    else
        q.z = positive ? 0u : edge - 1u;

    return
        LoadScalar(
            neighbor,
            q);
}

float3 WorldPosition(
    uint slot,
    uint3 local)
{
    const uint base =
        slot * 32u;

    const int3 tile =
        int3(
            asint(g_residency.Load(base + 0u)),
            asint(g_residency.Load(base + 4u)),
            asint(g_residency.Load(base + 8u)));

    const float3 cell =
        float3(
            g_pc.cellX,
            g_pc.cellY,
            g_pc.cellZ);

    return
        (float3(tile) *
             float(g_pc.tileEdge) +
         float3(local) +
         0.5) *
        cell;
}

float Influence(
    float3 worldPosition,
    uint inputIndex)
{
    const uint base =
        inputIndex * 64u;

    const float3 center =
        float3(
            asfloat(g_inputs.Load(base + 0u)),
            asfloat(g_inputs.Load(base + 4u)),
            asfloat(g_inputs.Load(base + 8u)));

    const float radius =
        max(
            asfloat(g_inputs.Load(base + 12u)),
            0.0001);

    const float3 halfExtent =
        max(
            float3(
                asfloat(g_inputs.Load(base + 32u)),
                asfloat(g_inputs.Load(base + 36u)),
                asfloat(g_inputs.Load(base + 40u))),
            0.0001);

    const uint shape =
        g_inputs.Load(base + 56u);

    if (shape == 0u ||
        shape == 1u)
    {
        return
            saturate(
                1.0 -
                length(
                    worldPosition - center) /
                    radius);
    }

    const float3 normalized =
        abs(
            worldPosition - center) /
        halfExtent;

    return
        saturate(
            1.0 -
            max(
                normalized.x,
                max(
                    normalized.y,
                    normalized.z)));
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_pc.totalCells)
        return;

    const uint slot =
        id.x /
        g_pc.tileCellCount;
    const uint localIndex =
        id.x -
        slot *
            g_pc.tileCellCount;

    const uint edge =
        g_pc.tileEdge;

    const uint3 local =
        uint3(
            localIndex % edge,
            (localIndex / edge) % edge,
            localIndex /
                (edge * edge));

    const float value =
        LoadScalar(
            slot,
            local);
    const float3 velocity =
        LoadVelocity(
            slot,
            local);

    const float xMinus =
        NeighborScalar(
            slot,
            local,
            0u,
            false);
    const float xPlus =
        NeighborScalar(
            slot,
            local,
            0u,
            true);
    const float yMinus =
        NeighborScalar(
            slot,
            local,
            1u,
            false);
    const float yPlus =
        NeighborScalar(
            slot,
            local,
            1u,
            true);
    const float zMinus =
        NeighborScalar(
            slot,
            local,
            2u,
            false);
    const float zPlus =
        NeighborScalar(
            slot,
            local,
            2u,
            true);

    const float dx =
        velocity.x >= 0.0
            ? (value - xMinus) /
                max(g_pc.cellX, 0.0001)
            : (xPlus - value) /
                max(g_pc.cellX, 0.0001);

    const float dy =
        velocity.y >= 0.0
            ? (value - yMinus) /
                max(g_pc.cellY, 0.0001)
            : (yPlus - value) /
                max(g_pc.cellY, 0.0001);

    const float dz =
        velocity.z >= 0.0
            ? (value - zMinus) /
                max(g_pc.cellZ, 0.0001)
            : (zPlus - value) /
                max(g_pc.cellZ, 0.0001);

    float solved =
        max(
            value -
            g_pc.dt *
                (velocity.x * dx +
                 velocity.y * dy +
                 velocity.z * dz),
            0.0);

    solved *=
        exp(
            -max(
                g_pc.dissipation,
                0.0) *
            g_pc.dt);

    const float3 worldPosition =
        WorldPosition(
            slot,
            local);

    [loop]
    for (uint inputIndex = 0u;
         inputIndex < g_pc.inputCount;
         ++inputIndex)
    {
        const uint base =
            inputIndex * 64u;

        const bool enabled =
            asfloat(
                g_inputs.Load(
                    base + 44u)) >
            0.5;

        if (!enabled)
            continue;

        const uint role =
            g_inputs.Load(
                base + 48u);
        const uint kind =
            g_inputs.Load(
                base + 52u);
        const uint fieldMask =
            g_inputs.Load(
                base + 60u);

        if ((fieldMask &
             g_pc.fieldBit) == 0u)
            continue;

        const float weight =
            Influence(
                worldPosition,
                inputIndex);

        if (weight <= 0.0)
            continue;

        const float scalarValue =
            asfloat(
                g_inputs.Load(
                    base + 28u));

        if (role == 0u)
        {
            solved +=
                scalarValue *
                g_pc.sourceScale *
                g_pc.dt *
                weight;
        }
        else
        {
            if (kind == 3u)
            {
                solved +=
                    scalarValue *
                    g_pc.dt *
                    weight;
            }
            else if (kind == 4u)
            {
                solved *=
                    exp(
                        -abs(scalarValue) *
                        g_pc.dt *
                        weight);
            }
        }
    }

    g_scalarOut.Store(
        ScalarAddress(
            slot,
            local),
        asuint(
            max(solved, 0.0)));
}
)";

[[nodiscard]] render_graph::BufferHandle
FindFieldHandle(
    const volume_fields::ImportedVolumeFields& fields,
    const world_model::VolumeField field)
{
    for (const auto& channel :
         fields.channels)
    {
        if (channel.field == field)
        {
            return channel.buffer;
        }
    }

    return {};
}

[[nodiscard]] u32 FieldBit(
    const world_model::VolumeField field) noexcept
{
    return
        static_cast<u32>(
            static_cast<u64>(field) &
            0xffffffffULL);
}
} // namespace

void StepSurfaceVolumeReference(
    const std::span<const SurfaceVolumeCell> input,
    const std::span<SurfaceVolumeCell> output,
    const SurfaceVolumeReferenceConfig& config,
    const math::Float3 uniformWind)
{
    const std::size_t count =
        static_cast<std::size_t>(
            config.width) *
        config.height *
        config.layers;

    if (config.width == 0U ||
        config.height == 0U ||
        config.layers == 0U ||
        input.size() != count ||
        output.size() != count ||
        config.cellSizeX <= 0.0F ||
        config.cellSizeY <= 0.0F ||
        config.cellSizeZ <= 0.0F ||
        config.deltaSeconds < 0.0F)
    {
        throw std::invalid_argument(
            "M33 reference solver configuration is invalid.");
    }

    const auto sample =
        [&](const i32 x,
            const i32 y,
            const i32 z)
            -> const SurfaceVolumeCell&
        {
            const u32 sx =
                static_cast<u32>(
                    std::clamp<i32>(
                        x,
                        0,
                        static_cast<i32>(
                            config.width) -
                            1));
            const u32 sy =
                static_cast<u32>(
                    std::clamp<i32>(
                        y,
                        0,
                        static_cast<i32>(
                            config.layers) -
                            1));
            const u32 sz =
                static_cast<u32>(
                    std::clamp<i32>(
                        z,
                        0,
                        static_cast<i32>(
                            config.height) -
                            1));

            return input[
                CpuIndex(
                    sx,
                    sy,
                    sz,
                    config)];
        };

    for (u32 z = 0U;
         z < config.height;
         ++z)
    {
        for (u32 y = 0U;
             y < config.layers;
             ++y)
        {
            for (u32 x = 0U;
                 x < config.width;
                 ++x)
            {
                const auto& center =
                    sample(
                        static_cast<i32>(x),
                        static_cast<i32>(y),
                        static_cast<i32>(z));

                const auto& xm =
                    sample(
                        static_cast<i32>(x) - 1,
                        static_cast<i32>(y),
                        static_cast<i32>(z));
                const auto& xp =
                    sample(
                        static_cast<i32>(x) + 1,
                        static_cast<i32>(y),
                        static_cast<i32>(z));
                const auto& ym =
                    sample(
                        static_cast<i32>(x),
                        static_cast<i32>(y) - 1,
                        static_cast<i32>(z));
                const auto& yp =
                    sample(
                        static_cast<i32>(x),
                        static_cast<i32>(y) + 1,
                        static_cast<i32>(z));
                const auto& zm =
                    sample(
                        static_cast<i32>(x),
                        static_cast<i32>(y),
                        static_cast<i32>(z) - 1);
                const auto& zp =
                    sample(
                        static_cast<i32>(x),
                        static_cast<i32>(y),
                        static_cast<i32>(z) + 1);

                const auto scalarGradient =
                    math::Float3{
                        center.velocity.x >= 0.0F
                            ? (center.scalar - xm.scalar) /
                                  config.cellSizeX
                            : (xp.scalar - center.scalar) /
                                  config.cellSizeX,
                        center.velocity.y >= 0.0F
                            ? (center.scalar - ym.scalar) /
                                  config.cellSizeY
                            : (yp.scalar - center.scalar) /
                                  config.cellSizeY,
                        center.velocity.z >= 0.0F
                            ? (center.scalar - zm.scalar) /
                                  config.cellSizeZ
                            : (zp.scalar - center.scalar) /
                                  config.cellSizeZ
                    };

                const f32 transportedScalar =
                    std::max(
                        center.scalar -
                            config.deltaSeconds *
                                (center.velocity.x *
                                     scalarGradient.x +
                                 center.velocity.y *
                                     scalarGradient.y +
                                 center.velocity.z *
                                     scalarGradient.z),
                        0.0F);

                const auto velocityDerivative =
                    [&](const math::Float3& minus,
                        const math::Float3& plus,
                        const f32 component,
                        const f32 spacing)
                    {
                        return
                            component >= 0.0F
                                ? (center.velocity -
                                   minus) /
                                      spacing
                                : (plus -
                                   center.velocity) /
                                      spacing;
                    };

                const auto dx =
                    velocityDerivative(
                        xm.velocity,
                        xp.velocity,
                        center.velocity.x,
                        config.cellSizeX);
                const auto dy =
                    velocityDerivative(
                        ym.velocity,
                        yp.velocity,
                        center.velocity.y,
                        config.cellSizeY);
                const auto dz =
                    velocityDerivative(
                        zm.velocity,
                        zp.velocity,
                        center.velocity.z,
                        config.cellSizeZ);

                auto velocity =
                    center.velocity -
                    (dx * center.velocity.x +
                     dy * center.velocity.y +
                     dz * center.velocity.z) *
                        config.deltaSeconds;

                velocity =
                    velocity *
                        std::exp(
                            -std::max(
                                config.velocityDissipationPerSecond,
                                0.0F) *
                            config.deltaSeconds) +
                    uniformWind *
                        config.deltaSeconds;

                output[
                    CpuIndex(
                        x,
                        y,
                        z,
                        config)] = {
                    .scalar =
                        Decay(
                            transportedScalar,
                            config.scalarDissipationPerSecond,
                            config.deltaSeconds),
                    .velocity =
                        velocity
                };
            }
        }
    }
}

class SurfaceVolumeSolverService::Impl
{
public:
    struct Scratch
    {
        world_model::VolumeField field{
            world_model::VolumeField::Density};
        u64 bytes{0U};
        std::unique_ptr<rhi::Buffer> buffer;
    };

    struct Entry
    {
        scene::ObjectId volume{};
        SurfaceVolumeSolverSettings settings{};
        SurfaceVolumeSolverDiagnostics diagnostics{};
        std::vector<Scratch> scratch;
        std::unique_ptr<rhi::Buffer> neighbors;
        std::unique_ptr<rhi::Buffer> inputs;
        u32 neighborCapacity{0U};
        u32 inputCapacity{0U};
        bool initialized{false};
    };

    Impl(
        rhi::Device& device,
        const shader::Compiler& compiler)
        : device(&device)
    {
        clearPipeline =
            CompileCompute(
                device,
                compiler,
                kClearShader,
                1U,
                1U);
        velocityPipeline =
            CompileCompute(
                device,
                compiler,
                kVelocityShader,
                12U,
                5U);
        scalarPipeline =
            CompileCompute(
                device,
                compiler,
                kScalarShader,
                12U,
                6U);
    }

    [[nodiscard]] Entry&
    EnsureEntry(
        const scene::ObjectId volume)
    {
        auto found =
            std::find_if(
                entries.begin(),
                entries.end(),
                [volume](const Entry& entry)
                {
                    return entry.volume ==
                        volume;
                });

        if (found == entries.end())
        {
            Entry entry;
            entry.volume = volume;
            entry.settings.resetRequested = true;
            entries.push_back(
                std::move(entry));
            return entries.back();
        }

        return *found;
    }

    void EnsureScratch(
        Entry& entry,
        const volume_fields::VolumeFieldDiagnostics& fields)
    {
        for (const auto& channel :
             fields.channels)
        {
            auto found =
                std::find_if(
                    entry.scratch.begin(),
                    entry.scratch.end(),
                    [&channel](const Scratch& scratch)
                    {
                        return scratch.field ==
                            channel.field;
                    });

            if (found != entry.scratch.end() &&
                found->bytes ==
                    channel.sizeBytes)
            {
                continue;
            }

            Scratch scratch{
                .field = channel.field,
                .bytes = channel.sizeBytes,
                .buffer =
                    device->CreateBuffer({
                        .sizeBytes =
                            channel.sizeBytes,
                        .usage =
                            rhi::BufferUsage::
                                Structured,
                        .memory =
                            rhi::MemoryUsage::
                                GpuOnly,
                        .initialState =
                            rhi::ResourceState::
                                ShaderResource
                    })
            };

            if (found !=
                entry.scratch.end())
            {
                *found =
                    std::move(scratch);
            }
            else
            {
                entry.scratch.push_back(
                    std::move(scratch));
            }
        }

        std::erase_if(
            entry.scratch,
            [&fields](const Scratch& scratch)
            {
                return std::none_of(
                    fields.channels.begin(),
                    fields.channels.end(),
                    [&scratch](
                        const volume_fields::
                            FieldChannelInfo& channel)
                    {
                        return channel.field ==
                            scratch.field;
                    });
            });
    }

    void UploadNeighbors(
        Entry& entry,
        const volume_fields::VolumeFieldStorage& storage)
    {
        const auto tiles =
            storage.Tiles();

        if (tiles.empty())
        {
            return;
        }

        if (entry.neighborCapacity <
            tiles.size())
        {
            entry.neighborCapacity =
                static_cast<u32>(
                    tiles.size());

            entry.neighbors =
                device->CreateBuffer({
                    .sizeBytes =
                        static_cast<u64>(
                            entry.neighborCapacity) *
                        sizeof(NeighborRecord),
                    .usage =
                        rhi::BufferUsage::
                            Structured,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
        }

        std::unordered_map<
            volume_fields::TileCoord,
            u32,
            TileCoordHash>
            slots;
        slots.reserve(
            tiles.size());

        for (const auto& tile :
             tiles)
        {
            if (tile.resident)
            {
                slots.emplace(
                    tile.coord,
                    tile.slot);
            }
        }

        auto* records =
            reinterpret_cast<
                NeighborRecord*>(
                    entry.neighbors->Map());

        for (const auto& tile :
             tiles)
        {
            const auto findSlot =
                [&slots](
                    const volume_fields::TileCoord coord)
                {
                    const auto found =
                        slots.find(coord);
                    return found != slots.end()
                        ? found->second
                        : kInvalidSlot;
                };

            records[tile.slot] = {
                .horizontal = {
                    findSlot({
                        tile.coord.x - 1,
                        tile.coord.y,
                        tile.coord.z}),
                    findSlot({
                        tile.coord.x + 1,
                        tile.coord.y,
                        tile.coord.z}),
                    findSlot({
                        tile.coord.x,
                        tile.coord.y,
                        tile.coord.z - 1}),
                    findSlot({
                        tile.coord.x,
                        tile.coord.y,
                        tile.coord.z + 1})
                },
                .vertical = {
                    findSlot({
                        tile.coord.x,
                        tile.coord.y - 1,
                        tile.coord.z}),
                    findSlot({
                        tile.coord.x,
                        tile.coord.y + 1,
                        tile.coord.z}),
                    0U,
                    0U
                }
            };
        }

        entry.neighbors->Unmap();
        entry.diagnostics.neighborRecordCount =
            static_cast<u32>(
                tiles.size());
    }

    void UploadInputs(
        Entry& entry,
        const std::span<const world_model::ResolvedVolumeInput> inputs)
    {
        const u32 count =
            std::min<u32>(
                static_cast<u32>(
                    inputs.size()),
                kMaximumInputs);
        const u32 required =
            std::max(
                count,
                1U);

        if (entry.inputCapacity <
            required)
        {
            entry.inputCapacity =
                std::max(
                    required,
                    std::max(
                        entry.inputCapacity * 2U,
                        8U));

            entry.inputs =
                device->CreateBuffer({
                    .sizeBytes =
                        static_cast<u64>(
                            entry.inputCapacity) *
                        sizeof(InputRecord),
                    .usage =
                        rhi::BufferUsage::
                            Structured,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
        }

        auto* records =
            reinterpret_cast<
                InputRecord*>(
                    entry.inputs->Map());

        std::fill_n(
            records,
            entry.inputCapacity,
            InputRecord{});

        u32 sources = 0U;
        u32 effectors = 0U;

        for (u32 index = 0U;
             index < count;
             ++index)
        {
            const auto& input =
                inputs[index];

            records[index] = {
                .positionRadius = {
                    static_cast<f32>(
                        input.positionMeters.x),
                    static_cast<f32>(
                        input.positionMeters.y),
                    static_cast<f32>(
                        input.positionMeters.z),
                    static_cast<f32>(
                        input.radiusMeters)
                },
                .vectorScalar = {
                    static_cast<f32>(
                        input.vectorValue.x),
                    static_cast<f32>(
                        input.vectorValue.y),
                    static_cast<f32>(
                        input.vectorValue.z),
                    static_cast<f32>(
                        input.scalarValue)
                },
                .halfEnabled = {
                    static_cast<f32>(
                        std::abs(
                            input.halfExtentsMeters.x)),
                    static_cast<f32>(
                        std::abs(
                            input.halfExtentsMeters.y)),
                    static_cast<f32>(
                        std::abs(
                            input.halfExtentsMeters.z)),
                    input.enabled
                        ? 1.0F
                        : 0.0F
                },
                .meta = {
                    static_cast<u32>(
                        input.role),
                    static_cast<u32>(
                        std::max<i64>(
                            input.kind,
                            0)),
                    static_cast<u32>(
                        input.shape),
                    static_cast<u32>(
                        input.fieldMask &
                        0xffffffffULL)
                }
            };

            if (input.role ==
                world_model::
                    VolumeInputRole::Source)
            {
                ++sources;
            }
            else
            {
                ++effectors;
            }
        }

        entry.inputs->Unmap();

        entry.diagnostics.sourceCount =
            sources;
        entry.diagnostics.effectorCount =
            effectors;
    }

    rhi::Device* device{nullptr};
    std::unique_ptr<rhi::ComputePipeline>
        clearPipeline;
    std::unique_ptr<rhi::ComputePipeline>
        velocityPipeline;
    std::unique_ptr<rhi::ComputePipeline>
        scalarPipeline;
    std::vector<Entry> entries;
};

SurfaceVolumeSolverService::SurfaceVolumeSolverService(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : impl_(
          std::make_unique<Impl>(
              device,
              compiler))
{
}

SurfaceVolumeSolverService::~SurfaceVolumeSolverService() =
    default;

SurfaceVolumeSolverSettings&
SurfaceVolumeSolverService::Settings(
    const scene::ObjectId volume)
{
    return
        impl_->EnsureEntry(
            volume).
            settings;
}

SurfaceVolumeSolverDiagnostics
SurfaceVolumeSolverService::Diagnostics(
    const scene::ObjectId volume) const noexcept
{
    const auto found =
        std::find_if(
            impl_->entries.begin(),
            impl_->entries.end(),
            [volume](const Impl::Entry& entry)
            {
                return entry.volume ==
                    volume;
            });

    return found != impl_->entries.end()
        ? found->diagnostics
        : SurfaceVolumeSolverDiagnostics{};
}

void SurfaceVolumeSolverService::RemoveMissing(
    const scene::ObjectStore& objects)
{
    std::erase_if(
        impl_->entries,
        [&objects](const Impl::Entry& entry)
        {
            const auto record =
                objects.Find(
                    entry.volume);
            return
                !record.has_value() ||
                record->type !=
                    world_model::kVolumeType;
        });
}

void SurfaceVolumeSolverService::AddPasses(
    render_graph::RenderGraph& graph,
    const std::string_view prefix,
    const scene::ObjectStore& objects,
    const world_model::ResolvedVolumeDomain& domain,
    volume_fields::VolumeFieldStorage& storage,
    const volume_fields::ImportedVolumeFields& fields)
{
    auto& entry =
        impl_->EnsureEntry(
            domain.object);

    entry.diagnostics = {
        .eligible =
            domain.enabled &&
            domain.solverPolicy ==
                world_model::
                    VolumeSolverPolicy::Surface2D5D,
        .live =
            entry.settings.live,
        .paused =
            entry.settings.paused,
        .simulatedSeconds =
            entry.diagnostics.simulatedSeconds
    };

    if (!entry.diagnostics.eligible)
    {
        return;
    }

    const auto& fieldDiagnostics =
        storage.Diagnostics();

    impl_->EnsureScratch(
        entry,
        fieldDiagnostics);
    impl_->UploadNeighbors(
        entry,
        storage);

    const auto inputs =
        world_model::ResolveVolumeInputs(
            objects,
            domain.object);

    impl_->UploadInputs(
        entry,
        inputs);

    entry.diagnostics.scratchBytes =
        0U;

    for (const auto& scratch :
         entry.scratch)
    {
        entry.diagnostics.scratchBytes +=
            scratch.bytes;
    }

    entry.diagnostics.metadataBytes =
        (entry.neighbors != nullptr
             ? entry.neighbors->SizeBytes()
             : 0U) +
        (entry.inputs != nullptr
             ? entry.inputs->SizeBytes()
             : 0U);

    const bool shouldStep =
        entry.settings.live &&
        (!entry.settings.paused ||
         entry.settings.singleStepRequested);

    const bool shouldReset =
        entry.settings.resetRequested ||
        !entry.initialized;

    std::vector<render_graph::BufferHandle>
        scratchHandles;

    scratchHandles.reserve(
        entry.scratch.size());

    for (auto& scratch :
         entry.scratch)
    {
        scratchHandles.push_back(
            graph.ImportBuffer(
                std::string(prefix) +
                    ".Scratch." +
                    std::to_string(
                        static_cast<u64>(
                            scratch.field)),
                *scratch.buffer,
                rhi::ResourceState::
                    ShaderResource));
    }

    const auto neighborHandle =
        graph.ImportBuffer(
            std::string(prefix) +
                ".Neighbors",
            *entry.neighbors,
            rhi::ResourceState::
                ShaderResource);

    const auto inputHandle =
        graph.ImportBuffer(
            std::string(prefix) +
                ".Inputs",
            *entry.inputs,
            rhi::ResourceState::
                ShaderResource);

    const u32 tileEdge =
        fieldDiagnostics.tileEdge;
    const u32 tileCellCount =
        tileEdge *
        tileEdge *
        tileEdge;
    const u32 totalCells =
        fieldDiagnostics.residentTiles *
        tileCellCount;

    const auto clearField =
        [&](const world_model::VolumeField field)
        {
            const auto handle =
                FindFieldHandle(
                    fields,
                    field);

            if (!handle.IsValid())
                return;

            const auto* channel =
                std::find_if(
                    fieldDiagnostics.channels.begin(),
                    fieldDiagnostics.channels.end(),
                    [field](const auto& item)
                    {
                        return item.field ==
                            field;
                    });

            if (channel ==
                fieldDiagnostics.channels.end())
                return;

            graph.AddPass(
                std::string(prefix) +
                    ".Reset." +
                    std::to_string(
                        static_cast<u64>(field)),
                {},
                {
                    {
                        .buffer = handle,
                        .state =
                            rhi::ResourceState::
                                UnorderedAccess,
                        .access =
                            render_graph::Access::
                                Write
                    }
                },
                [this,
                 handle,
                 dwords =
                     static_cast<u32>(
                         channel->sizeBytes /
                         4U)](
                    rhi::CommandList& commands,
                    const render_graph::Resources& resources)
                {
                    commands.SetComputePipeline(
                        *impl_->clearPipeline);
                    commands.SetComputeBuffer(
                        0U,
                        resources.Buffer(handle));

                    const std::array<u32,1>
                        constants{dwords};

                    commands.SetComputeConstants(
                        constants);
                    commands.Dispatch(
                        (dwords +
                         kThreadGroupSize -
                         1U) /
                            kThreadGroupSize,
                        1U,
                        1U);
                });
        };

    if (shouldReset)
    {
        for (const auto& channel :
             fieldDiagnostics.channels)
        {
            clearField(
                channel.field);
        }

        entry.settings.resetRequested =
            false;
        entry.initialized =
            true;
        entry.diagnostics.resetThisFrame =
            true;
    }

    if (!shouldStep)
    {
        entry.settings.singleStepRequested =
            false;
        return;
    }

    const auto velocityHandle =
        FindFieldHandle(
            fields,
            world_model::
                VolumeField::Velocity);

    if (!velocityHandle.IsValid())
    {
        entry.settings.singleStepRequested =
            false;
        return;
    }

    const auto scratchFor =
        [&](const world_model::VolumeField field)
            -> render_graph::BufferHandle
        {
            for (std::size_t index = 0U;
                 index < entry.scratch.size();
                 ++index)
            {
                if (entry.scratch[index].field ==
                    field)
                {
                    return
                        scratchHandles[index];
                }
            }

            return {};
        };

    const f32 cellX =
        static_cast<f32>(
            domain.halfExtentsMeters.x *
            2.0 /
            static_cast<f64>(
                std::max(
                    fieldDiagnostics.resolutionX,
                    1U)));
    const f32 cellY =
        static_cast<f32>(
            domain.halfExtentsMeters.y *
            2.0 /
            static_cast<f64>(
                std::max(
                    fieldDiagnostics.resolutionY,
                    1U)));
    const f32 cellZ =
        static_cast<f32>(
            domain.halfExtentsMeters.z *
            2.0 /
            static_cast<f64>(
                std::max(
                    fieldDiagnostics.resolutionZ,
                    1U)));

    const u32 inputCount =
        std::min<u32>(
            static_cast<u32>(
                inputs.size()),
            kMaximumInputs);

    const u32 iterations =
        std::clamp(
            entry.settings.iterationsPerFrame,
            1U,
            16U);

    const f32 dt =
        std::clamp(
            entry.settings.timeStepSeconds,
            1.0F / 1000.0F,
            0.1F);

    for (u32 iteration = 0U;
         iteration < iterations;
         ++iteration)
    {
        const auto velocityScratch =
            scratchFor(
                world_model::
                    VolumeField::Velocity);

        if (!velocityScratch.IsValid())
        {
            break;
        }

        graph.AddPass(
            std::string(prefix) +
                ".Velocity." +
                std::to_string(iteration),
            {},
            {
                {velocityHandle,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
                {fields.residency,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
                {neighborHandle,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
                {inputHandle,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
                {velocityScratch,rhi::ResourceState::UnorderedAccess,render_graph::Access::Write}
            },
            [this,
             velocityHandle,
             residency = fields.residency,
             neighborHandle,
             inputHandle,
             velocityScratch,
             totalCells,
             tileEdge,
             tileCellCount,
             inputCount,
             dt,
             cellX,
             cellY,
             cellZ,
             dissipation =
                 entry.settings.
                     velocityDissipationPerSecond,
             sourceScale =
                 entry.settings.sourceScale](
                rhi::CommandList& commands,
                const render_graph::Resources& resources)
            {
                std::array<u32,12> pc{
                    totalCells,
                    tileEdge,
                    tileCellCount,
                    inputCount,
                    std::bit_cast<u32>(dt),
                    std::bit_cast<u32>(cellX),
                    std::bit_cast<u32>(cellY),
                    std::bit_cast<u32>(cellZ),
                    std::bit_cast<u32>(dissipation),
                    std::bit_cast<u32>(sourceScale),
                    FieldBit(world_model::VolumeField::Velocity),
                    0U
                };

                commands.SetComputePipeline(
                    *impl_->velocityPipeline);
                commands.SetComputeBuffer(
                    0U,
                    resources.Buffer(
                        velocityHandle));
                commands.SetComputeBuffer(
                    1U,
                    resources.Buffer(
                        residency));
                commands.SetComputeBuffer(
                    2U,
                    resources.Buffer(
                        neighborHandle));
                commands.SetComputeBuffer(
                    3U,
                    resources.Buffer(
                        inputHandle));
                commands.SetComputeBuffer(
                    4U,
                    resources.Buffer(
                        velocityScratch));
                commands.SetComputeConstants(
                    pc);
                commands.Dispatch(
                    (totalCells +
                     kThreadGroupSize -
                     1U) /
                        kThreadGroupSize,
                    1U,
                    1U);
            });

        graph.AddPass(
            std::string(prefix) +
                ".VelocityCommit." +
                std::to_string(iteration),
            {},
            {
                {velocityScratch,rhi::ResourceState::CopySource,render_graph::Access::Read},
                {velocityHandle,rhi::ResourceState::CopyDestination,render_graph::Access::Write}
            },
            [velocityScratch,
             velocityHandle](
                rhi::CommandList& commands,
                const render_graph::Resources& resources)
            {
                auto& source =
                    resources.Buffer(
                        velocityScratch);
                auto& target =
                    resources.Buffer(
                        velocityHandle);

                commands.CopyBuffer(
                    source,
                    0U,
                    target,
                    0U,
                    std::min(
                        source.SizeBytes(),
                        target.SizeBytes()));
            });

        for (const auto& channel :
             fieldDiagnostics.channels)
        {
            if (channel.field ==
                world_model::
                    VolumeField::Velocity)
            {
                continue;
            }

            const auto scalar =
                FindFieldHandle(
                    fields,
                    channel.field);
            const auto scalarScratch =
                scratchFor(
                    channel.field);

            if (!scalar.IsValid() ||
                !scalarScratch.IsValid())
            {
                continue;
            }

            graph.AddPass(
                std::string(prefix) +
                    ".Scalar." +
                    std::to_string(
                        static_cast<u64>(
                            channel.field)) +
                    "." +
                    std::to_string(
                        iteration),
                {},
                {
                    {scalar,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
                    {velocityHandle,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
                    {fields.residency,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
                    {neighborHandle,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
                    {inputHandle,rhi::ResourceState::ShaderResource,render_graph::Access::Read},
                    {scalarScratch,rhi::ResourceState::UnorderedAccess,render_graph::Access::Write}
                },
                [this,
                 scalar,
                 velocityHandle,
                 residency = fields.residency,
                 neighborHandle,
                 inputHandle,
                 scalarScratch,
                 field = channel.field,
                 totalCells,
                 tileEdge,
                 tileCellCount,
                 inputCount,
                 dt,
                 cellX,
                 cellY,
                 cellZ,
                 dissipation =
                     entry.settings.
                         scalarDissipationPerSecond,
                 sourceScale =
                     entry.settings.sourceScale](
                    rhi::CommandList& commands,
                    const render_graph::Resources& resources)
                {
                    std::array<u32,12> pc{
                        totalCells,
                        tileEdge,
                        tileCellCount,
                        inputCount,
                        std::bit_cast<u32>(dt),
                        std::bit_cast<u32>(cellX),
                        std::bit_cast<u32>(cellY),
                        std::bit_cast<u32>(cellZ),
                        std::bit_cast<u32>(dissipation),
                        std::bit_cast<u32>(sourceScale),
                        FieldBit(field),
                        0U
                    };

                    commands.SetComputePipeline(
                        *impl_->scalarPipeline);
                    commands.SetComputeBuffer(
                        0U,
                        resources.Buffer(scalar));
                    commands.SetComputeBuffer(
                        1U,
                        resources.Buffer(
                            velocityHandle));
                    commands.SetComputeBuffer(
                        2U,
                        resources.Buffer(
                            residency));
                    commands.SetComputeBuffer(
                        3U,
                        resources.Buffer(
                            neighborHandle));
                    commands.SetComputeBuffer(
                        4U,
                        resources.Buffer(
                            inputHandle));
                    commands.SetComputeBuffer(
                        5U,
                        resources.Buffer(
                            scalarScratch));
                    commands.SetComputeConstants(
                        pc);
                    commands.Dispatch(
                        (totalCells +
                         kThreadGroupSize -
                         1U) /
                            kThreadGroupSize,
                        1U,
                        1U);
                });

            graph.AddPass(
                std::string(prefix) +
                    ".ScalarCommit." +
                    std::to_string(
                        static_cast<u64>(
                            channel.field)) +
                    "." +
                    std::to_string(
                        iteration),
                {},
                {
                    {scalarScratch,rhi::ResourceState::CopySource,render_graph::Access::Read},
                    {scalar,rhi::ResourceState::CopyDestination,render_graph::Access::Write}
                },
                [scalarScratch,
                 scalar](
                    rhi::CommandList& commands,
                    const render_graph::Resources& resources)
                {
                    auto& source =
                        resources.Buffer(
                            scalarScratch);
                    auto& target =
                        resources.Buffer(
                            scalar);

                    commands.CopyBuffer(
                        source,
                        0U,
                        target,
                        0U,
                        std::min(
                            source.SizeBytes(),
                            target.SizeBytes()));
                });

            ++entry.diagnostics.
                scalarChannelsSolved;
        }
    }

    storage.MarkAllResidentTilesValid();

    entry.settings.singleStepRequested =
        false;
    entry.diagnostics.steppedThisFrame =
        true;
    entry.diagnostics.iterationsThisFrame =
        iterations;
    entry.diagnostics.simulatedSeconds +=
        dt *
        static_cast<f32>(
            iterations);
}

std::string_view
SurfaceVolumeDebugViewName(
    const SurfaceVolumeDebugView view) noexcept
{
    switch (view)
    {
    case SurfaceVolumeDebugView::Off:
        return "Off";
    case SurfaceVolumeDebugView::Density:
        return "Density";
    case SurfaceVolumeDebugView::Velocity:
        return "Velocity";
    }

    return "Unknown";
}
} // namespace orbit::volume_solver
