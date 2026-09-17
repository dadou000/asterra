#include <orbit/terrain_gpu/GpuMaterialColumnResources.hpp>

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

[[nodiscard]] std::unique_ptr<rhi::Texture> CreateLane(
    rhi::Device& device,
    const u32 resolution,
    const rhi::TextureFormat format)
{
    return device.CreateTexture({
        .width = resolution,
        .height = resolution,
        .format = format,
        .initialState = rhi::ResourceState::CopyDestination,
        .allowUnorderedAccess = true
    });
}
} // namespace

GpuMaterialColumnResources::GpuMaterialColumnResources(
    rhi::Device& device,
    const u32 resolution)
    : resolution_(resolution)
{
    if (resolution_ == 0)
    {
        throw std::invalid_argument(
            "M08 GPU material page requires nonzero resolution.");
    }

    const u64 texels =
        static_cast<u64>(resolution_) *
        static_cast<u64>(resolution_);

    bedrockHeight_ =
        CreateLane(
            device,
            resolution_,
            rhi::TextureFormat::R32_Float);
    looseMaterials_ =
        CreateLane(
            device,
            resolution_,
            rhi::TextureFormat::RGBA16_Float);
    moistureProcess_ =
        CreateLane(
            device,
            resolution_,
            rhi::TextureFormat::RG16_Float);
    geologicalMaterial_ =
        CreateLane(
            device,
            resolution_,
            rhi::TextureFormat::R16_UInt);

    bedrockStaging_ =
        CreateStaging(
            device,
            texels * sizeof(f32));
    looseStaging_ =
        CreateStaging(
            device,
            texels *
                sizeof(
                    terrain_material_column::
                        GpuLooseMaterialTexel));
    moistureStaging_ =
        CreateStaging(
            device,
            texels *
                sizeof(
                    terrain_material_column::
                        GpuMoistureProcessTexel));
    materialStaging_ =
        CreateStaging(
            device,
            texels * sizeof(u16));
}

void GpuMaterialColumnResources::UploadLane(
    rhi::CommandList& commandList,
    rhi::Buffer& staging,
    const void* const data,
    const u64 sizeBytes,
    rhi::Texture& texture)
{
    if (staging.SizeBytes() != sizeBytes)
    {
        throw std::logic_error(
            "M08 staging lane size does not match packed CPU data.");
    }

    std::byte* mapped = staging.Map();
    std::memcpy(
        mapped,
        data,
        static_cast<std::size_t>(sizeBytes));
    staging.Unmap();

    if (uploaded_)
    {
        commandList.Transition(
            texture,
            rhi::ResourceState::UnorderedAccess,
            rhi::ResourceState::CopyDestination);
    }

    commandList.CopyBufferToTexture(
        staging,
        0,
        texture);

    commandList.Transition(
        texture,
        rhi::ResourceState::CopyDestination,
        rhi::ResourceState::UnorderedAccess);
}

void GpuMaterialColumnResources::Upload(
    rhi::CommandList& commandList,
    const terrain_material_column::GpuMaterialColumnPage& page)
{
    if (page.resolution != resolution_)
    {
        throw std::invalid_argument(
            "M08 packed CPU page resolution does not match GPU page.");
    }

    const std::size_t expected =
        static_cast<std::size_t>(resolution_) *
        static_cast<std::size_t>(resolution_);

    if (page.bedrockHeightR32F.size() != expected ||
        page.looseRgba16F.size() != expected ||
        page.moistureProcessRg16F.size() != expected ||
        page.geologicalMaterialR16Uint.size() != expected)
    {
        throw std::invalid_argument(
            "M08 packed CPU page has incomplete texture lanes.");
    }

    UploadLane(
        commandList,
        *bedrockStaging_,
        page.bedrockHeightR32F.data(),
        static_cast<u64>(
            page.bedrockHeightR32F.size() *
            sizeof(f32)),
        *bedrockHeight_);

    UploadLane(
        commandList,
        *looseStaging_,
        page.looseRgba16F.data(),
        static_cast<u64>(
            page.looseRgba16F.size() *
            sizeof(
                terrain_material_column::
                    GpuLooseMaterialTexel)),
        *looseMaterials_);

    UploadLane(
        commandList,
        *moistureStaging_,
        page.moistureProcessRg16F.data(),
        static_cast<u64>(
            page.moistureProcessRg16F.size() *
            sizeof(
                terrain_material_column::
                    GpuMoistureProcessTexel)),
        *moistureProcess_);

    UploadLane(
        commandList,
        *materialStaging_,
        page.geologicalMaterialR16Uint.data(),
        static_cast<u64>(
            page.geologicalMaterialR16Uint.size() *
            sizeof(u16)),
        *geologicalMaterial_);

    uploaded_ = true;
}

u32 GpuMaterialColumnResources::Resolution() const noexcept
{
    return resolution_;
}

rhi::Texture&
GpuMaterialColumnResources::BedrockHeight() noexcept
{
    return *bedrockHeight_;
}

rhi::Texture&
GpuMaterialColumnResources::LooseMaterials() noexcept
{
    return *looseMaterials_;
}

rhi::Texture&
GpuMaterialColumnResources::MoistureProcess() noexcept
{
    return *moistureProcess_;
}

rhi::Texture&
GpuMaterialColumnResources::GeologicalMaterial() noexcept
{
    return *geologicalMaterial_;
}
} // namespace orbit::terrain_gpu
