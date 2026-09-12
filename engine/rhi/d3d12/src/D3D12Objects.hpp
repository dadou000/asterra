#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <orbit/rhi/Device.hpp>

#include <memory>
#include <string>
#include <vector>

namespace orbit::rhi::d3d12::detail
{
using Microsoft::WRL::ComPtr;

[[nodiscard]] D3D12_COMMAND_LIST_TYPE ToNativeQueueType(QueueType type);
[[nodiscard]] D3D12_RESOURCE_STATES ToNativeResourceState(ResourceState state);

class D3D12Fence final : public Fence
{
public:
    D3D12Fence(ComPtr<ID3D12Fence> nativeFence, HANDLE eventHandle);
    ~D3D12Fence() override;

    [[nodiscard]] u64 CompletedValue() const noexcept override;
    void Wait(u64 value) override;

    [[nodiscard]] ID3D12Fence* Native() const noexcept;

private:
    ComPtr<ID3D12Fence> nativeFence_;
    HANDLE eventHandle_{nullptr};
};

class D3D12Queue final : public Queue
{
public:
    D3D12Queue(QueueType type, ComPtr<ID3D12CommandQueue> nativeQueue);

    [[nodiscard]] QueueType Type() const noexcept override;
    void Submit(CommandList& commandList) override;
    void Signal(Fence& fence, u64 value) override;

    [[nodiscard]] ID3D12CommandQueue* Native() const noexcept;

private:
    QueueType type_;
    ComPtr<ID3D12CommandQueue> nativeQueue_;
};

class D3D12Texture final : public Texture
{
public:
    D3D12Texture(
        ComPtr<ID3D12Resource> nativeResource,
        u32 width,
        u32 height,
        D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView);

    [[nodiscard]] u32 Width() const noexcept override;
    [[nodiscard]] u32 Height() const noexcept override;

    [[nodiscard]] ID3D12Resource* Native() const noexcept;
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView() const noexcept;

private:
    ComPtr<ID3D12Resource> nativeResource_;
    u32 width_{};
    u32 height_{};
    D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView_{};
};

class D3D12CommandAllocator final : public CommandAllocator
{
public:
    D3D12CommandAllocator(
        QueueType type,
        ComPtr<ID3D12CommandAllocator> nativeAllocator);

    [[nodiscard]] QueueType Type() const noexcept override;
    void Reset() override;

    [[nodiscard]] ID3D12CommandAllocator* Native() const noexcept;

private:
    QueueType type_;
    ComPtr<ID3D12CommandAllocator> nativeAllocator_;
};

class D3D12CommandList final : public CommandList
{
public:
    D3D12CommandList(
        QueueType type,
        ComPtr<ID3D12GraphicsCommandList> nativeCommandList);

    [[nodiscard]] QueueType Type() const noexcept override;

    void Reset(CommandAllocator& allocator) override;
    void Transition(
        Texture& texture,
        ResourceState before,
        ResourceState after) override;
    void ClearColorTarget(Texture& texture, const ClearColor& color) override;
    void Close() override;

    [[nodiscard]] ID3D12GraphicsCommandList* Native() const noexcept;

private:
    QueueType type_;
    ComPtr<ID3D12GraphicsCommandList> nativeCommandList_;
};

class D3D12Swapchain final : public Swapchain
{
public:
    D3D12Swapchain(
        ComPtr<IDXGISwapChain4> nativeSwapchain,
        ID3D12Device& device,
        const SwapchainDesc& desc,
        bool tearingEnabled);

    void Present(bool verticalSync) override;

    [[nodiscard]] u32 Width() const noexcept override;
    [[nodiscard]] u32 Height() const noexcept override;
    [[nodiscard]] u32 BufferCount() const noexcept override;
    [[nodiscard]] u32 CurrentBackBufferIndex() const noexcept override;
    [[nodiscard]] Texture& CurrentBackBuffer() noexcept override;

private:
    ComPtr<IDXGISwapChain4> nativeSwapchain_;
    ComPtr<ID3D12DescriptorHeap> renderTargetHeap_;
    std::vector<std::unique_ptr<D3D12Texture>> backBuffers_;
    u32 width_{};
    u32 height_{};
    u32 bufferCount_{};
    bool tearingEnabled_{false};
};

class D3D12Device final : public Device
{
public:
    D3D12Device(
        ComPtr<ID3D12Device> nativeDevice,
        ComPtr<IDXGIFactory6> factory,
        std::string adapterName,
        DeviceCapabilities capabilities);

    [[nodiscard]] Backend GetBackend() const noexcept override;
    [[nodiscard]] std::string_view AdapterName() const noexcept override;
    [[nodiscard]] const DeviceCapabilities& Capabilities() const noexcept override;

    [[nodiscard]] std::unique_ptr<Queue> CreateQueue(QueueType type) override;
    [[nodiscard]] std::unique_ptr<Fence> CreateFence(u64 initialValue) override;
    [[nodiscard]] std::unique_ptr<CommandAllocator> CreateCommandAllocator(
        QueueType type) override;
    [[nodiscard]] std::unique_ptr<CommandList> CreateCommandList(
        CommandAllocator& allocator) override;
    [[nodiscard]] std::unique_ptr<Swapchain> CreateSwapchain(
        Queue& queue,
        const SwapchainDesc& desc) override;

private:
    ComPtr<ID3D12Device> nativeDevice_;
    ComPtr<IDXGIFactory6> factory_;
    std::string adapterName_;
    DeviceCapabilities capabilities_{};
};

} // namespace orbit::rhi::d3d12::detail
