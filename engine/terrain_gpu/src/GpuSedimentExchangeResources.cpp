#include <orbit/terrain_gpu/GpuSedimentExchangeResources.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace orbit::terrain_gpu
{
namespace
{
[[nodiscard]] std::unique_ptr<rhi::Buffer> CreateStaging(
    rhi::Device& device,
    const u64 sizeBytes)
{
    return device.CreateBuffer({
        .sizeBytes = sizeBytes,
        .usage = rhi::BufferUsage::Generic,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::CopySource
    });
}

[[nodiscard]] std::unique_ptr<rhi::Buffer> CreateGpuLane(
    rhi::Device& device,
    const u64 sizeBytes)
{
    return device.CreateBuffer({
        .sizeBytes = sizeBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::CopyDestination
    });
}

[[nodiscard]] terrain_erosion::SedimentMass UnpackMedium(
    const terrain_erosion::GpuSedimentMediumTexel& texel,
    const f64 area) noexcept
{
    return {
        .sandKg =
            static_cast<f64>(
                texel.
                    sandKgPerSquareMeter) *
            area,
        .finesKg =
            static_cast<f64>(
                texel.
                    finesKgPerSquareMeter) *
            area,
        .coarseDebrisKg =
            static_cast<f64>(
                texel.
                    coarseDebrisKgPerSquareMeter) *
            area
    };
}
} // namespace

GpuSedimentExchangeResources::
GpuSedimentExchangeResources(
    rhi::Device& device,
    const u32 resolution)
    : resolution_(resolution)
{
    if (resolution_ == 0U)
    {
        throw std::invalid_argument(
            "M14 GPU sediment exchange requires nonzero resolution.");
    }

    const u64 cellCount =
        static_cast<u64>(
            resolution_) *
        resolution_;

    const u64 laneBytes =
        cellCount *
        sizeof(
            terrain_erosion::
                GpuSedimentMediumTexel);

    waterborne_ =
        CreateGpuLane(
            device,
            laneBytes);

    airborne_ =
        CreateGpuLane(
            device,
            laneBytes);

    surfaceMobile_ =
        CreateGpuLane(
            device,
            laneBytes);

    waterborneStaging_ =
        CreateStaging(
            device,
            laneBytes);

    airborneStaging_ =
        CreateStaging(
            device,
            laneBytes);

    surfaceMobileStaging_ =
        CreateStaging(
            device,
            laneBytes);

    readback_ =
        device.CreateBuffer({
            .sizeBytes =
                laneBytes * 3U,
            .usage =
                rhi::BufferUsage::Generic,
            .memory =
                rhi::MemoryUsage::HostReadback,
            .initialState =
                rhi::ResourceState::CopyDestination
        });
}

GpuSedimentExchangeResources::
~GpuSedimentExchangeResources() = default;

void GpuSedimentExchangeResources::UploadLane(
    rhi::CommandList& commandList,
    rhi::Buffer& staging,
    const void* const data,
    const u64 sizeBytes,
    rhi::Buffer& destination)
{
    if (staging.SizeBytes() !=
            sizeBytes ||
        destination.SizeBytes() !=
            sizeBytes)
    {
        throw std::logic_error(
            "M14 GPU sediment lane size does not match packed CPU data.");
    }

    std::byte* mapped =
        staging.Map();

    std::memcpy(
        mapped,
        data,
        static_cast<std::size_t>(
            sizeBytes));

    staging.Unmap();

    if (uploaded_)
    {
        commandList.Transition(
            destination,
            rhi::ResourceState::UnorderedAccess,
            rhi::ResourceState::CopyDestination);
    }

    commandList.CopyBuffer(
        staging,
        0U,
        destination,
        0U,
        sizeBytes);

    commandList.Transition(
        destination,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::UnorderedAccess);
}

void GpuSedimentExchangeResources::Upload(
    rhi::CommandList& commandList,
    const terrain_erosion::GpuSedimentExchangePage& page)
{
    if (page.resolution !=
        resolution_)
    {
        throw std::invalid_argument(
            "M14 packed sediment page resolution does not match GPU resources.");
    }

    const std::size_t expected =
        static_cast<std::size_t>(
            resolution_) *
        resolution_;

    if (page.waterborne.size() !=
            expected ||
        page.airborne.size() !=
            expected ||
        page.surfaceMobile.size() !=
            expected)
    {
        throw std::invalid_argument(
            "M14 packed sediment page has incomplete GPU lanes.");
    }

    const u64 laneBytes =
        static_cast<u64>(
            expected) *
        sizeof(
            terrain_erosion::
                GpuSedimentMediumTexel);

    UploadLane(
        commandList,
        *waterborneStaging_,
        page.waterborne.data(),
        laneBytes,
        *waterborne_);

    UploadLane(
        commandList,
        *airborneStaging_,
        page.airborne.data(),
        laneBytes,
        *airborne_);

    UploadLane(
        commandList,
        *surfaceMobileStaging_,
        page.surfaceMobile.data(),
        laneBytes,
        *surfaceMobile_);

    uploaded_ = true;
    readbackRecorded_ = false;
}

void GpuSedimentExchangeResources::RecordReadback(
    rhi::CommandList& commandList)
{
    if (!uploaded_)
    {
        throw std::logic_error(
            "M14 GPU sediment readback requires uploaded/initialized state.");
    }

    const u64 laneBytes =
        waterborne_->
            SizeBytes();

    const std::array<rhi::Buffer*, 3>
        lanes{
            waterborne_.get(),
            airborne_.get(),
            surfaceMobile_.get()
        };

    for (u32 i = 0U;
         i < lanes.size();
         ++i)
    {
        rhi::Buffer& lane =
            *lanes[i];

        commandList.Transition(
            lane,
            rhi::ResourceState::UnorderedAccess,
            rhi::ResourceState::CopySource);

        commandList.CopyBuffer(
            lane,
            0U,
            *readback_,
            static_cast<u64>(i) *
                laneBytes,
            laneBytes);

        commandList.Transition(
            lane,
            rhi::ResourceState::CopySource,
            rhi::ResourceState::UnorderedAccess);
    }

    readbackRecorded_ = true;
}

void GpuSedimentExchangeResources::ApplyReadbackToCpu(
    terrain_erosion::SedimentExchangePage& page)
{
    if (!readbackRecorded_ ||
        page.Resolution() !=
            resolution_)
    {
        throw std::logic_error(
            "M14 CPU sediment sync requires a completed matching readback.");
    }

    const u64 cellCount =
        static_cast<u64>(
            resolution_) *
        resolution_;

    const u64 laneBytes =
        cellCount *
        sizeof(
            terrain_erosion::
                GpuSedimentMediumTexel);

    const f64 area =
        page.SpacingMeters() *
        page.SpacingMeters();

    const std::byte* mapped =
        readback_->Map();

    for (u32 y = 0U;
         y < resolution_;
         ++y)
    {
        for (u32 x = 0U;
             x < resolution_;
             ++x)
        {
            const u64 index =
                static_cast<u64>(y) *
                    resolution_ +
                x;

            std::array<
                terrain_erosion::
                    GpuSedimentMediumTexel,
                3>
                packed{};

            for (u32 medium = 0U;
                 medium <
                     packed.size();
                 ++medium)
            {
                std::memcpy(
                    &packed[medium],
                    mapped +
                        static_cast<u64>(
                            medium) *
                            laneBytes +
                        index *
                            sizeof(
                                terrain_erosion::
                                    GpuSedimentMediumTexel),
                    sizeof(
                        terrain_erosion::
                            GpuSedimentMediumTexel));
            }

            auto& cell =
                page.At(
                    x,
                    y);

            cell.waterborne =
                UnpackMedium(
                    packed[0],
                    area);

            cell.airborne =
                UnpackMedium(
                    packed[1],
                    area);

            cell.surfaceMobile =
                UnpackMedium(
                    packed[2],
                    area);
        }
    }

    readback_->Unmap();

    readbackRecorded_ = false;
}

u32 GpuSedimentExchangeResources::Resolution() const noexcept
{
    return resolution_;
}

rhi::Buffer&
GpuSedimentExchangeResources::Waterborne() noexcept
{
    return *waterborne_;
}

rhi::Buffer&
GpuSedimentExchangeResources::Airborne() noexcept
{
    return *airborne_;
}

rhi::Buffer&
GpuSedimentExchangeResources::SurfaceMobile() noexcept
{
    return *surfaceMobile_;
}
} // namespace orbit::terrain_gpu
