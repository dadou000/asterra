#include <orbit/terrain_gpu/GpuRegionDelta.hpp>

#include "RegionDeltaCompute.hpp"

#include <orbit/math/Vector.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

namespace orbit::terrain_gpu
{
namespace
{
constexpr u32 kThreadGroupSize = 8;

// 6 float4 fields (24 dwords) followed by 13 scalar fields -- see
// RegionDeltaCompute.hpp's PushConstants struct comment for why the
// vectors come first (no implicit HLSL alignment padding to
// reproduce here, unlike FieldGenerationCompute.hpp's push constants,
// which mix scalars and vectors and need each vector's dword offset
// computed by hand).
constexpr u32 kPushConstantDwords = 24 + 13;

void StoreFloat3(
    std::array<u32, kPushConstantDwords>& out,
    const u32 index,
    const math::Double3& v)
{
    out[index + 0] = std::bit_cast<u32>(static_cast<f32>(v.x));
    out[index + 1] = std::bit_cast<u32>(static_cast<f32>(v.y));
    out[index + 2] = std::bit_cast<u32>(static_cast<f32>(v.z));
    // out[index + 3] (the float4's .w) stays zero-initialized unless the
    // caller explicitly uses it for packed scalar data.
}
} // namespace

GpuRegionDelta::GpuRegionDelta(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler)
{
    const shader::Binary compute = shaderCompiler.Compile({
        .source = detail::kRegionDeltaComputeShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });

    if (compute.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile the region-delta compute "
            "shader.");
    }

    pipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = compute.bytecode.data(),
            .size = compute.bytecode.size()
        },
        .pushConstantDwords = kPushConstantDwords,
        .shaderResourceBuffers = 2
    });
}

GpuRegionDelta::~GpuRegionDelta() = default;

void GpuRegionDelta::Dispatch(
    rhi::CommandList& commandList,
    const GpuRegionDeltaRequest& request,
    rhi::Buffer& regionDelta,
    rhi::Buffer& samples) const
{
    std::array<u32, kPushConstantDwords> pushConstants{};

    StoreFloat3(pushConstants, 0, request.surfaceFrame.up);
    pushConstants[3] = std::bit_cast<u32>(
        static_cast<f32>(request.centerOffsetMeters.x));
    StoreFloat3(pushConstants, 4, request.surfaceFrame.east);
    pushConstants[7] = std::bit_cast<u32>(
        static_cast<f32>(request.centerOffsetMeters.y));
    StoreFloat3(pushConstants, 8, request.surfaceFrame.north);
    StoreFloat3(pushConstants, 12, request.regionSurfaceFrame.up);
    StoreFloat3(pushConstants, 16, request.regionSurfaceFrame.east);
    StoreFloat3(pushConstants, 20, request.regionSurfaceFrame.north);

    u32 i = 24;
    pushConstants[i++] = request.resolution;
    pushConstants[i++] =
        std::bit_cast<u32>(static_cast<f32>(request.spacingMeters));
    pushConstants[i++] = request.originX;
    pushConstants[i++] = request.originY;
    pushConstants[i++] = request.region.x;
    pushConstants[i++] = request.region.y;
    pushConstants[i++] = request.region.width;
    pushConstants[i++] = request.region.height;
    pushConstants[i++] = std::bit_cast<u32>(
        static_cast<f32>(request.planetRadiusMeters));
    pushConstants[i++] = std::bit_cast<u32>(
        static_cast<f32>(request.regionHalfExtentMeters));
    pushConstants[i++] = std::bit_cast<u32>(
        static_cast<f32>(request.regionSpacingMeters));
    pushConstants[i++] = request.regionResolution;
    pushConstants[i++] = std::bit_cast<u32>(request.edgeFadeStartDot);

    const u32 groupCountX =
        (request.region.width + kThreadGroupSize - 1) / kThreadGroupSize;
    const u32 groupCountY =
        (request.region.height + kThreadGroupSize - 1) /
        kThreadGroupSize;

    commandList.SetComputePipeline(*pipeline_);
    commandList.SetComputeBuffer(0, regionDelta);
    commandList.SetComputeBuffer(1, samples);
    commandList.SetComputeConstants(pushConstants);
    commandList.Dispatch(
        std::max(groupCountX, 1U), std::max(groupCountY, 1U), 1);
}
} // namespace orbit::terrain_gpu
