#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/terrain_erosion/SedimentExchange.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
// Persistent GPU mirror of the M14 shared mobile-sediment page.
//
// Three float4 buffers are kept separate so hydraulic, aeolian and gravity/
// glacial passes can bind only the medium they need:
//   xyz = sand / fines / coarse debris kg/m2
//   w   = reserved
//
// CPU and GPU can hand authority back explicitly through Upload and readback;
// process-local ping-pong buffers remain scratch and are not persistent
// inter-process state.
class GpuSedimentExchangeResources
{
public:
    GpuSedimentExchangeResources(
        rhi::Device& device,
        u32 resolution);
    ~GpuSedimentExchangeResources();

    GpuSedimentExchangeResources(
        const GpuSedimentExchangeResources&) = delete;
    GpuSedimentExchangeResources& operator=(
        const GpuSedimentExchangeResources&) = delete;

    void Upload(
        rhi::CommandList& commandList,
        const terrain_erosion::GpuSedimentExchangePage& page);

    // Records all three GPU media into one persistent host-readback buffer.
    // ApplyReadbackToCpu() must only be called after the submission fence for
    // this command list has completed.
    void RecordReadback(
        rhi::CommandList& commandList);

    void ApplyReadbackToCpu(
        terrain_erosion::SedimentExchangePage& page);

    [[nodiscard]] u32 Resolution() const noexcept;

    [[nodiscard]] rhi::Buffer& Waterborne() noexcept;
    [[nodiscard]] rhi::Buffer& Airborne() noexcept;
    [[nodiscard]] rhi::Buffer& SurfaceMobile() noexcept;

private:
    void UploadLane(
        rhi::CommandList& commandList,
        rhi::Buffer& staging,
        const void* data,
        u64 sizeBytes,
        rhi::Buffer& destination);

    u32 resolution_{0};
    bool uploaded_{false};
    bool readbackRecorded_{false};

    std::unique_ptr<rhi::Buffer> waterborne_;
    std::unique_ptr<rhi::Buffer> airborne_;
    std::unique_ptr<rhi::Buffer> surfaceMobile_;

    std::unique_ptr<rhi::Buffer> waterborneStaging_;
    std::unique_ptr<rhi::Buffer> airborneStaging_;
    std::unique_ptr<rhi::Buffer> surfaceMobileStaging_;

    std::unique_ptr<rhi::Buffer> readback_;
};
} // namespace orbit::terrain_gpu
