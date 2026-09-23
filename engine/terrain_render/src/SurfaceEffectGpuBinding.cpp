#include <orbit/terrain_render/SurfaceEffectGpuBinding.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace orbit::terrain_render
{
SurfaceEffectGpuBinding::SurfaceEffectGpuBinding(
    rhi::Device& device,
    const u32 framesInFlight)
{
    if (framesInFlight == 0U)
    {
        throw std::invalid_argument(
            "Orbit M38 surface-effect binding requires at least one frame in flight.");
    }

    frameBuffers_.reserve(framesInFlight);

    for (u32 frameIndex = 0U;
         frameIndex < framesInFlight;
         ++frameIndex)
    {
        frameBuffers_.push_back(
            device.CreateBuffer({
                .sizeBytes = sizeof(StampArray),
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::HostVisible,
                .initialState = rhi::ResourceState::ShaderResource
            }));
    }
}

void SurfaceEffectGpuBinding::Set(
    const std::span<const SurfaceEffectGpuStamp> effects)
{
    snapshot_.fill({});

    activeStampCount_ =
        static_cast<u32>(
            std::min<std::size_t>(
                effects.size(),
                MaximumStampCount));

    std::copy_n(
        effects.begin(),
        activeStampCount_,
        snapshot_.begin());
}

void SurfaceEffectGpuBinding::Bind(
    rhi::CommandList& commands,
    const u32 frameIndex)
{
    if (frameIndex >= frameBuffers_.size())
    {
        throw std::out_of_range(
            "Orbit M38 surface-effect frame index exceeds configured frames in flight.");
    }

    rhi::Buffer& buffer = *frameBuffers_[frameIndex];
    std::byte* mapped = buffer.Map();
    std::memcpy(
        mapped,
        snapshot_.data(),
        sizeof(snapshot_));
    buffer.Unmap();

    commands.SetGraphicsBuffer(
        GraphicsBufferSlot,
        buffer);
}

u32 SurfaceEffectGpuBinding::ActiveStampCount() const noexcept
{
    return activeStampCount_;
}
} // namespace orbit::terrain_render
