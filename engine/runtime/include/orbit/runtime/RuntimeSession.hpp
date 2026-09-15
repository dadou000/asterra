#pragma once

#include <orbit/core/Types.hpp>

#include <memory>
#include <string_view>

namespace orbit::platform
{
class Window;
}

namespace orbit::rhi
{
class Device;
class Queue;
class Swapchain;
}

namespace orbit::runtime
{
struct RuntimeSessionDesc
{
    std::string_view applicationName{"Orbit"};
    std::string_view windowTitle{"Orbit"};
    u32 width{1600};
    u32 height{900};
    u32 swapchainBufferCount{3};
    bool allowTearing{true};
    bool relativeMouseMode{false};
};

class RuntimeSession
{
public:
    explicit RuntimeSession(const RuntimeSessionDesc& desc);
    ~RuntimeSession();

    RuntimeSession(const RuntimeSession&) = delete;
    RuntimeSession& operator=(const RuntimeSession&) = delete;
    RuntimeSession(RuntimeSession&&) = delete;
    RuntimeSession& operator=(RuntimeSession&&) = delete;

    [[nodiscard]] platform::Window& Window() noexcept;
    [[nodiscard]] const platform::Window& Window() const noexcept;

    [[nodiscard]] rhi::Device& Device() noexcept;
    [[nodiscard]] const rhi::Device& Device() const noexcept;

    [[nodiscard]] rhi::Queue& GraphicsQueue() noexcept;
    [[nodiscard]] const rhi::Queue& GraphicsQueue() const noexcept;

    [[nodiscard]] rhi::Swapchain& Swapchain() noexcept;
    [[nodiscard]] const rhi::Swapchain& Swapchain() const noexcept;

    // Resizes the swapchain to the current non-zero client size.
    // Returns true only when a resize actually happened. Swapchain-sized
    // resources owned by higher-level render systems must be recreated by
    // those systems after a true result.
    [[nodiscard]] bool ResizeSwapchainToWindow();

private:
    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<rhi::Device> device_;
    std::unique_ptr<rhi::Queue> graphicsQueue_;
    std::unique_ptr<rhi::Swapchain> swapchain_;
};
} // namespace orbit::runtime
