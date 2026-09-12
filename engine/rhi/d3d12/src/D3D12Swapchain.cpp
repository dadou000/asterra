#include "D3D12Objects.hpp"

#include <stdexcept>
#include <utility>

namespace orbit::rhi::d3d12::detail
{
D3D12Swapchain::D3D12Swapchain(
    ComPtr<IDXGISwapChain4> nativeSwapchain,
    ID3D12Device& device,
    const SwapchainDesc& desc,
    const bool tearingEnabled)
    : nativeSwapchain_(std::move(nativeSwapchain)),
      width_(desc.width),
      height_(desc.height),
      bufferCount_(desc.bufferCount),
      tearingEnabled_(tearingEnabled)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = bufferCount_;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    if (FAILED(device.CreateDescriptorHeap(
            &heapDesc,
            IID_PPV_ARGS(&renderTargetHeap_))))
    {
        throw std::runtime_error(
            "Orbit failed to create the swapchain RTV descriptor heap.");
    }

    const u32 descriptorSize =
        device.GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE handle =
        renderTargetHeap_->GetCPUDescriptorHandleForHeapStart();

    backBuffers_.reserve(bufferCount_);

    for (u32 index = 0; index < bufferCount_; ++index)
    {
        ComPtr<ID3D12Resource> resource;
        if (FAILED(nativeSwapchain_->GetBuffer(
                index,
                IID_PPV_ARGS(&resource))))
        {
            throw std::runtime_error(
                "Orbit failed to acquire a D3D12 swapchain backbuffer.");
        }

        device.CreateRenderTargetView(
            resource.Get(),
            nullptr,
            handle);

        backBuffers_.push_back(
            std::make_unique<D3D12Texture>(
                std::move(resource),
                width_,
                height_,
                handle));

        handle.ptr += descriptorSize;
    }
}

void D3D12Swapchain::Present(const bool verticalSync)
{
    const UINT syncInterval = verticalSync ? 1U : 0U;
    const UINT flags =
        (!verticalSync && tearingEnabled_)
            ? DXGI_PRESENT_ALLOW_TEARING
            : 0U;

    if (FAILED(nativeSwapchain_->Present(
            syncInterval,
            flags)))
    {
        throw std::runtime_error(
            "Orbit failed to present the D3D12 swapchain.");
    }
}

u32 D3D12Swapchain::Width() const noexcept
{
    return width_;
}

u32 D3D12Swapchain::Height() const noexcept
{
    return height_;
}

u32 D3D12Swapchain::BufferCount() const noexcept
{
    return bufferCount_;
}

u32 D3D12Swapchain::CurrentBackBufferIndex() const noexcept
{
    return nativeSwapchain_->GetCurrentBackBufferIndex();
}

Texture& D3D12Swapchain::CurrentBackBuffer() noexcept
{
    return *backBuffers_[CurrentBackBufferIndex()];
}
} // namespace orbit::rhi::d3d12::detail
