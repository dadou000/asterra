#pragma once

#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/terrain_render/SurfaceEffects.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace orbit::terrain_render
{
class SurfaceEffectGpuBinding
{
public:
    static constexpr u32 MaximumStampCount = 512U;
    static constexpr u32 GraphicsBufferSlot = 1U;

    SurfaceEffectGpuBinding(
        rhi::Device& device,
        u32 framesInFlight);

    void Set(
        std::span<const SurfaceEffectGpuStamp> effects);

    // Uploads the latest presentation snapshot into the selected
    // frame-in-flight resource and binds it at the shader's M38 SRV slot.
    // Unused entries are zero-filled; angularRadiusRadians == 0 is the shader
    // sentinel, so the common sparse case exits without scanning 512 entries.
    void Bind(
        rhi::CommandList& commands,
        u32 frameIndex);

    [[nodiscard]] u32 ActiveStampCount() const noexcept;

private:
    using StampArray =
        std::array<SurfaceEffectGpuStamp, MaximumStampCount>;

    StampArray snapshot_{};
    u32 activeStampCount_{0U};
    std::vector<std::unique_ptr<rhi::Buffer>> frameBuffers_;
};
} // namespace orbit::terrain_render
