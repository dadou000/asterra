#include <orbit/volume_render/VolumeParticleGpuBinding.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace orbit::volume_render
{
VolumeParticleGpuBinding::VolumeParticleGpuBinding(
    rhi::Device& device,
    const u32 framesInFlight)
{
    if (framesInFlight == 0U)
    {
        throw std::invalid_argument(
            "Orbit M38 particle binding requires at least one frame in flight.");
    }

    frameBuffers_.reserve(framesInFlight);
    for (u32 frameIndex = 0U; frameIndex < framesInFlight; ++frameIndex)
    {
        frameBuffers_.push_back(
            device.CreateBuffer({
                .sizeBytes = sizeof(SpawnArray),
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::HostVisible,
                .initialState = rhi::ResourceState::ShaderResource
            }));
    }
}

void VolumeParticleGpuBinding::Set(
    const std::span<const VolumeParticleGpuSpawn> spawns)
{
    snapshot_.fill({});
    activeSpawnCount_ = static_cast<u32>(
        std::min<std::size_t>(spawns.size(), MaximumSpawnCount));
    std::copy_n(spawns.begin(), activeSpawnCount_, snapshot_.begin());
}

void VolumeParticleGpuBinding::Bind(
    rhi::CommandList& commands,
    const u32 frameIndex)
{
    if (frameIndex >= frameBuffers_.size())
    {
        throw std::out_of_range(
            "Orbit M38 particle frame index exceeds configured frames in flight.");
    }

    rhi::Buffer& buffer = *frameBuffers_[frameIndex];
    std::byte* mapped = buffer.Map();
    std::memcpy(mapped, snapshot_.data(), sizeof(snapshot_));
    buffer.Unmap();
    commands.SetGraphicsBuffer(GraphicsBufferSlot, buffer);
}

u32 VolumeParticleGpuBinding::ActiveSpawnCount() const noexcept
{
    return activeSpawnCount_;
}
} // namespace orbit::volume_render
