#include "D3D12Objects.hpp"

#include <stdexcept>
#include <utility>

namespace orbit::rhi::d3d12::detail
{
D3D12_RESOURCE_STATES ToNativeResourceState(const ResourceState state)
{
    switch (state)
    {
    case ResourceState::Common:
        return D3D12_RESOURCE_STATE_COMMON;
    case ResourceState::Present:
        return D3D12_RESOURCE_STATE_PRESENT;
    case ResourceState::RenderTarget:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case ResourceState::DepthWrite:
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case ResourceState::DepthRead:
        return D3D12_RESOURCE_STATE_DEPTH_READ;
    case ResourceState::VertexOrConstantBuffer:
        return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    case ResourceState::IndexBuffer:
        return D3D12_RESOURCE_STATE_INDEX_BUFFER;
    case ResourceState::ShaderResource:
        return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    case ResourceState::UnorderedAccess:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case ResourceState::CopySource:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case ResourceState::CopyDestination:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    }

    throw std::runtime_error(
        "Orbit received an invalid RHI resource state.");
}

D3D12Texture::D3D12Texture(
    ComPtr<ID3D12Resource> nativeResource,
    const u32 width,
    const u32 height,
    const D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView)
    : nativeResource_(std::move(nativeResource)),
      width_(width),
      height_(height),
      renderTargetView_(renderTargetView)
{
}

u32 D3D12Texture::Width() const noexcept
{
    return width_;
}

u32 D3D12Texture::Height() const noexcept
{
    return height_;
}

ID3D12Resource* D3D12Texture::Native() const noexcept
{
    return nativeResource_.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Texture::RenderTargetView() const noexcept
{
    return renderTargetView_;
}

D3D12CommandAllocator::D3D12CommandAllocator(
    const QueueType type,
    ComPtr<ID3D12CommandAllocator> nativeAllocator)
    : type_(type),
      nativeAllocator_(std::move(nativeAllocator))
{
}

QueueType D3D12CommandAllocator::Type() const noexcept
{
    return type_;
}

void D3D12CommandAllocator::Reset()
{
    if (FAILED(nativeAllocator_->Reset()))
    {
        throw std::runtime_error(
            "Orbit failed to reset a D3D12 command allocator.");
    }
}

ID3D12CommandAllocator* D3D12CommandAllocator::Native() const noexcept
{
    return nativeAllocator_.Get();
}

D3D12CommandList::D3D12CommandList(
    const QueueType type,
    ComPtr<ID3D12GraphicsCommandList> nativeCommandList)
    : type_(type),
      nativeCommandList_(std::move(nativeCommandList))
{
}

QueueType D3D12CommandList::Type() const noexcept
{
    return type_;
}

void D3D12CommandList::Reset(CommandAllocator& allocator)
{
    auto* d3dAllocator =
        dynamic_cast<D3D12CommandAllocator*>(&allocator);

    if (d3dAllocator == nullptr ||
        d3dAllocator->Type() != type_)
    {
        throw std::runtime_error(
            "Orbit cannot reset a command list with an incompatible allocator.");
    }

    if (FAILED(nativeCommandList_->Reset(
            d3dAllocator->Native(),
            nullptr)))
    {
        throw std::runtime_error(
            "Orbit failed to reset a D3D12 command list.");
    }
}

void D3D12CommandList::Transition(
    Texture& texture,
    const ResourceState before,
    const ResourceState after)
{
    if (before == after)
    {
        return;
    }

    auto* d3dTexture = dynamic_cast<D3D12Texture*>(&texture);
    if (d3dTexture == nullptr)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a texture from another backend.");
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = d3dTexture->Native();
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore =
        ToNativeResourceState(before);
    barrier.Transition.StateAfter =
        ToNativeResourceState(after);

    nativeCommandList_->ResourceBarrier(1, &barrier);
}

void D3D12CommandList::ClearColorTarget(
    Texture& texture,
    const ClearColor& color)
{
    auto* d3dTexture = dynamic_cast<D3D12Texture*>(&texture);
    if (d3dTexture == nullptr)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a texture from another backend.");
    }

    const FLOAT clearColor[4] = {
        color.red,
        color.green,
        color.blue,
        color.alpha
    };

    nativeCommandList_->ClearRenderTargetView(
        d3dTexture->RenderTargetView(),
        clearColor,
        0,
        nullptr);
}

void D3D12CommandList::Close()
{
    if (FAILED(nativeCommandList_->Close()))
    {
        throw std::runtime_error(
            "Orbit failed to close a D3D12 command list.");
    }
}

ID3D12GraphicsCommandList* D3D12CommandList::Native() const noexcept
{
    return nativeCommandList_.Get();
}
} // namespace orbit::rhi::d3d12::detail
