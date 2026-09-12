#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Resource.hpp>

namespace orbit::rhi
{
struct SwapchainDesc
{
    void* nativeWindow{nullptr};
    u32 width{0};
    u32 height{0};
    u32 bufferCount{3};
    bool allowTearing{true};
};

class Swapchain
{
public:
    virtual ~Swapchain() = default;

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    virtual void Present(bool verticalSync) = 0;

    [[nodiscard]] virtual u32 Width() const noexcept = 0;
    [[nodiscard]] virtual u32 Height() const noexcept = 0;
    [[nodiscard]] virtual u32 BufferCount() const noexcept = 0;
    [[nodiscard]] virtual u32 CurrentBackBufferIndex() const noexcept = 0;
    [[nodiscard]] virtual Texture& CurrentBackBuffer() noexcept = 0;

protected:
    Swapchain() = default;
};
} // namespace orbit::rhi
