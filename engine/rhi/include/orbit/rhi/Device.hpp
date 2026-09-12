#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Fence.hpp>
#include <orbit/rhi/Queue.hpp>
#include <orbit/rhi/Swapchain.hpp>

#include <memory>
#include <string_view>

namespace orbit::rhi
{
enum class Backend : u8
{
    D3D12,
    Vulkan
};

struct DeviceCapabilities
{
    bool rayTracing{false};
    bool meshShaders{false};
    bool variableRateShading{false};
    bool presentTearing{false};
    u32 shaderModelMajor{0};
    u32 shaderModelMinor{0};
};

class Device
{
public:
    virtual ~Device() = default;

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] virtual Backend GetBackend() const noexcept = 0;
    [[nodiscard]] virtual std::string_view AdapterName() const noexcept = 0;
    [[nodiscard]] virtual const DeviceCapabilities& Capabilities() const noexcept = 0;

    [[nodiscard]] virtual std::unique_ptr<Queue> CreateQueue(QueueType type) = 0;
    [[nodiscard]] virtual std::unique_ptr<Fence> CreateFence(u64 initialValue) = 0;
    [[nodiscard]] virtual std::unique_ptr<CommandAllocator> CreateCommandAllocator(
        QueueType type) = 0;
    [[nodiscard]] virtual std::unique_ptr<CommandList> CreateCommandList(
        CommandAllocator& allocator) = 0;
    [[nodiscard]] virtual std::unique_ptr<Swapchain> CreateSwapchain(
        Queue& queue,
        const SwapchainDesc& desc) = 0;

protected:
    Device() = default;
};
} // namespace orbit::rhi
