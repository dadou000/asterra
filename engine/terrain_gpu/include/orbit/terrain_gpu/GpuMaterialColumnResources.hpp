#pragma once

#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Resource.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
// Owns the four mutable GPU texture lanes defined by V0.0.4 M08.
// CPU MaterialColumnPage remains authority; this object is a derived GPU page.
class GpuMaterialColumnResources
{
public:
    GpuMaterialColumnResources(
        rhi::Device& device,
        u32 resolution);

    GpuMaterialColumnResources(
        const GpuMaterialColumnResources&) = delete;
    GpuMaterialColumnResources& operator=(
        const GpuMaterialColumnResources&) = delete;

    // Records uploads into commandList. The object owns persistent staging
    // buffers, so their Vulkan resources remain alive until submission.
    // On return all four textures are in UnorderedAccess state, ready for
    // later M09+ compute passes.
    void Upload(
        rhi::CommandList& commandList,
        const terrain_material_column::GpuMaterialColumnPage& page);

    [[nodiscard]] u32 Resolution() const noexcept;

    [[nodiscard]] rhi::Texture& BedrockHeight() noexcept;
    [[nodiscard]] rhi::Texture& LooseMaterials() noexcept;
    [[nodiscard]] rhi::Texture& MoistureProcess() noexcept;
    [[nodiscard]] rhi::Texture& GeologicalMaterial() noexcept;

private:
    void UploadLane(
        rhi::CommandList& commandList,
        rhi::Buffer& staging,
        const void* data,
        u64 sizeBytes,
        rhi::Texture& texture);

    u32 resolution_{0};
    bool uploaded_{false};

    std::unique_ptr<rhi::Texture> bedrockHeight_;
    std::unique_ptr<rhi::Texture> looseMaterials_;
    std::unique_ptr<rhi::Texture> moistureProcess_;
    std::unique_ptr<rhi::Texture> geologicalMaterial_;

    std::unique_ptr<rhi::Buffer> bedrockStaging_;
    std::unique_ptr<rhi::Buffer> looseStaging_;
    std::unique_ptr<rhi::Buffer> moistureStaging_;
    std::unique_ptr<rhi::Buffer> materialStaging_;
};
} // namespace orbit::terrain_gpu
