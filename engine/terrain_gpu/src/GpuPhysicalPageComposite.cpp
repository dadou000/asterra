#include <orbit/terrain_gpu/GpuPhysicalPageComposite.hpp>

#include "PhysicalPageCompositeCompute.hpp"

#include <orbit/world/Planet.hpp>

#include <array>
#include <chrono>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace orbit::terrain_gpu
{
bool GpuPhysicalSurfacePage::IsValid() const noexcept
{
    return
        address.planet.IsValid() &&
        resolution >= 2U &&
        samples != nullptr &&
        samples->SizeBytes() >=
            static_cast<u64>(resolution) *
            resolution *
            sizeof(
                GpuPhysicalSurfaceTexel);
}

void GpuPhysicalPageComposite::PrewarmShaders(const shader::Compiler& compiler)
{
    static_cast<void>(compiler.Compile({
        .source = detail::kPhysicalPageCompositeShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    }));
}

GpuPhysicalPageComposite::
GpuPhysicalPageComposite(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : device_(&device)
{
    const auto compute =
        compiler.Compile({
            .source =
                detail::
                    kPhysicalPageCompositeShader,
            .entryPoint = "main",
            .stage =
                shader::Stage::Compute,
            .debug = false
        });

    if (compute.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile the M12 physical-page composite shader.");
    }

    shaderBytecode_ = compute.bytecode;
    static_cast<void>(Ready());
}

bool GpuPhysicalPageComposite::Ready() const
{
    if (pipeline_) return true;
    if (!pendingPipeline_.valid())
    {
        auto admitted = device_->TryCreateComputePipelineAsync({
            .computeShader = {shaderBytecode_.data(), shaderBytecode_.size()},
            .pushConstantDwords = 32U,
            .shaderResourceBuffers = 2U
        });
        if (admitted) pendingPipeline_ = std::move(*admitted);
    }
    if (!pendingPipeline_.valid() ||
        pendingPipeline_.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready)
        return false;
    pipeline_ = pendingPipeline_.get();
    return pipeline_ != nullptr;
}

GpuPhysicalPageComposite::
~GpuPhysicalPageComposite() = default;

void GpuPhysicalPageComposite::Dispatch(
    rhi::CommandList& commandList,
    const terrain_stream::TerrainSampleRequest& request,
    const terrain_stream::PhysicalRegion& region,
    const GpuPhysicalSurfacePage& page,
    const f64 planetRadiusMeters,
    rhi::Buffer& outputSamples) const
{
    if (!Ready())
        throw std::runtime_error(
            "Physical page composite used before pipeline publication.");
    if (!page.IsValid() ||
        request.resolution == 0U ||
        region.width == 0U ||
        region.height == 0U ||
        !std::isfinite(
            planetRadiusMeters) ||
        planetRadiusMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Orbit M12 physical-page composite request is invalid.");
    }

    const auto bounds =
        world::TileBounds(
            page.address.tile);

    std::array<u32, 32U>
        constants{};

    const auto storeFloat =
        [&](const u32 index,
            const f64 value)
        {
            constants[index] =
                std::bit_cast<u32>(
                    static_cast<f32>(
                        value));
        };

    const auto storeFloat3 =
        [&](const u32 index,
            const math::Double3& value)
        {
            storeFloat(
                index + 0U,
                value.x);
            storeFloat(
                index + 1U,
                value.y);
            storeFloat(
                index + 2U,
                value.z);
        };

    storeFloat3(
        0U,
        request.surfaceFrame.up);
    storeFloat(
        3U,
        request.
            centerOffsetMeters.x);

    storeFloat3(
        4U,
        request.surfaceFrame.east);
    storeFloat(
        7U,
        request.
            centerOffsetMeters.y);

    storeFloat3(
        8U,
        request.surfaceFrame.north);
    storeFloat(
        11U,
        planetRadiusMeters);

    storeFloat(
        12U,
        bounds.minimumUv.x);
    storeFloat(
        13U,
        bounds.minimumUv.y);
    storeFloat(
        14U,
        bounds.maximumUv.x);
    storeFloat(
        15U,
        bounds.maximumUv.y);

    constants[16U] =
        request.resolution;
    constants[17U] =
        request.originX;
    constants[18U] =
        request.originY;
    constants[19U] =
        page.resolution;

    constants[20U] =
        region.x;
    constants[21U] =
        region.y;
    constants[22U] =
        region.width;
    constants[23U] =
        region.height;

    constants[24U] =
        static_cast<u32>(
            page.address.
                tile.face);
    constants[25U] =
        page.address.
            tile.level;
    constants[26U] =
        page.address.
            tile.x;
    constants[27U] =
        page.address.
            tile.y;

    storeFloat(
        28U,
        request.spacingMeters);
    storeFloat(
        29U,
        request.morphToCoarser
            ? request.
                  coarseSpacingMeters
            : 0.0);
    storeFloat(
        30U,
        request.
            morphStartHalfExtentMeters);
    storeFloat(
        31U,
        request.
            morphEndHalfExtentMeters);

    commandList.SetComputePipeline(
        *pipeline_);

    commandList.SetComputeBuffer(
        0U,
        *page.samples);

    commandList.SetComputeBuffer(
        1U,
        outputSamples);

    commandList.SetComputeConstants(
        constants);

    constexpr u32 kThreadGroupSize =
        8U;

    commandList.Dispatch(
        (region.width +
         kThreadGroupSize -
         1U) /
            kThreadGroupSize,
        (region.height +
         kThreadGroupSize -
         1U) /
            kThreadGroupSize,
        1U);
}
} // namespace orbit::terrain_gpu
