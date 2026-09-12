#include "D3D12Objects.hpp"

#include <stdexcept>
#include <utility>

namespace orbit::rhi::d3d12::detail
{
D3D12Fence::D3D12Fence(
    ComPtr<ID3D12Fence> nativeFence,
    const HANDLE eventHandle)
    : nativeFence_(std::move(nativeFence)),
      eventHandle_(eventHandle)
{
}

D3D12Fence::~D3D12Fence()
{
    if (eventHandle_ != nullptr)
    {
        CloseHandle(eventHandle_);
    }
}

u64 D3D12Fence::CompletedValue() const noexcept
{
    return nativeFence_->GetCompletedValue();
}

void D3D12Fence::Wait(const u64 value)
{
    if (CompletedValue() >= value)
    {
        return;
    }

    if (FAILED(nativeFence_->SetEventOnCompletion(
            value,
            eventHandle_)))
    {
        throw std::runtime_error(
            "Orbit failed to arm a D3D12 fence event.");
    }

    const DWORD result = WaitForSingleObject(
        eventHandle_,
        INFINITE);

    if (result != WAIT_OBJECT_0)
    {
        throw std::runtime_error(
            "Orbit failed while waiting for a D3D12 fence.");
    }
}

ID3D12Fence* D3D12Fence::Native() const noexcept
{
    return nativeFence_.Get();
}

D3D12Queue::D3D12Queue(
    const QueueType type,
    ComPtr<ID3D12CommandQueue> nativeQueue)
    : type_(type),
      nativeQueue_(std::move(nativeQueue))
{
}

QueueType D3D12Queue::Type() const noexcept
{
    return type_;
}

void D3D12Queue::Submit(CommandList& commandList)
{
    auto* d3dCommandList =
        dynamic_cast<D3D12CommandList*>(&commandList);

    if (d3dCommandList == nullptr ||
        d3dCommandList->Type() != type_)
    {
        throw std::runtime_error(
            "Orbit cannot submit a command list to an incompatible queue.");
    }

    ID3D12CommandList* lists[] = {
        d3dCommandList->Native()
    };

    nativeQueue_->ExecuteCommandLists(1, lists);
}

void D3D12Queue::Signal(Fence& fence, const u64 value)
{
    auto* d3dFence = dynamic_cast<D3D12Fence*>(&fence);
    if (d3dFence == nullptr)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a fence from another backend.");
    }

    if (FAILED(nativeQueue_->Signal(
            d3dFence->Native(),
            value)))
    {
        throw std::runtime_error(
            "Orbit failed to signal a D3D12 fence.");
    }
}

ID3D12CommandQueue* D3D12Queue::Native() const noexcept
{
    return nativeQueue_.Get();
}
} // namespace orbit::rhi::d3d12::detail
