#include "D3D12Objects.hpp"

#include <stdexcept>

namespace orbit::rhi::d3d12::detail
{
D3D12_RESOURCE_STATES ToNativeResourceState(
    const ResourceState state)
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

ID3D12CommandAllocator*
D3D12CommandAllocator::Native() const noexcept
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

void D3D12CommandList::Reset(
    CommandAllocator& allocator)
{
    auto* d3dAllocator =
        dynamic_cast<D3D12CommandAllocator*>(
            &allocator);

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

    activePipeline_ = nullptr;
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

    auto* d3dTexture =
        dynamic_cast<D3D12Texture*>(&texture);

    if (d3dTexture == nullptr)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a texture from another backend.");
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type =
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags =
        D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource =
        d3dTexture->Native();
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore =
        ToNativeResourceState(before);
    barrier.Transition.StateAfter =
        ToNativeResourceState(after);

    nativeCommandList_->ResourceBarrier(
        1,
        &barrier);
}

void D3D12CommandList::Transition(
    Buffer& buffer,
    const ResourceState before,
    const ResourceState after)
{
    if (before == after)
    {
        return;
    }

    auto* d3dBuffer =
        dynamic_cast<D3D12Buffer*>(&buffer);

    if (d3dBuffer == nullptr)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a buffer from another backend.");
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type =
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags =
        D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource =
        d3dBuffer->Native();
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore =
        ToNativeResourceState(before);
    barrier.Transition.StateAfter =
        ToNativeResourceState(after);

    nativeCommandList_->ResourceBarrier(
        1,
        &barrier);
}

void D3D12CommandList::CopyBuffer(
    Buffer& source,
    const u64 sourceOffsetBytes,
    Buffer& destination,
    const u64 destinationOffsetBytes,
    const u64 sizeBytes)
{
    auto* d3dSource =
        dynamic_cast<D3D12Buffer*>(&source);

    auto* d3dDestination =
        dynamic_cast<D3D12Buffer*>(&destination);

    if (d3dSource == nullptr ||
        d3dDestination == nullptr)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a buffer from another backend.");
    }

    if (sizeBytes == 0)
    {
        return;
    }

    if (sourceOffsetBytes >
            source.SizeBytes() ||
        sizeBytes >
            source.SizeBytes() -
                sourceOffsetBytes ||
        destinationOffsetBytes >
            destination.SizeBytes() ||
        sizeBytes >
            destination.SizeBytes() -
                destinationOffsetBytes)
    {
        throw std::out_of_range(
            "Orbit buffer copy exceeds source or destination bounds.");
    }

    nativeCommandList_->CopyBufferRegion(
        d3dDestination->Native(),
        destinationOffsetBytes,
        d3dSource->Native(),
        sourceOffsetBytes,
        sizeBytes);
}

void D3D12CommandList::ClearColorTarget(
    Texture& texture,
    const ClearColor& color)
{
    auto* d3dTexture =
        dynamic_cast<D3D12Texture*>(&texture);

    if (d3dTexture == nullptr ||
        d3dTexture->RenderTargetView().ptr == 0)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a texture without a render-target view.");
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

void D3D12CommandList::ClearDepthTarget(
    Texture& texture,
    const f32 depth)
{
    auto* d3dTexture =
        dynamic_cast<D3D12Texture*>(&texture);

    if (d3dTexture == nullptr ||
        d3dTexture->DepthStencilView().ptr == 0)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a texture without a depth-stencil view.");
    }

    nativeCommandList_->ClearDepthStencilView(
        d3dTexture->DepthStencilView(),
        D3D12_CLEAR_FLAG_DEPTH,
        depth,
        0,
        0,
        nullptr);
}

void D3D12CommandList::SetRenderTarget(
    Texture& texture)
{
    auto* d3dTexture =
        dynamic_cast<D3D12Texture*>(&texture);

    if (d3dTexture == nullptr ||
        d3dTexture->RenderTargetView().ptr == 0)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a texture without a render-target view.");
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE handle =
        d3dTexture->RenderTargetView();

    nativeCommandList_->OMSetRenderTargets(
        1,
        &handle,
        FALSE,
        nullptr);
}

void D3D12CommandList::SetRenderTargets(
    Texture& color,
    Texture& depth)
{
    auto* d3dColor =
        dynamic_cast<D3D12Texture*>(&color);

    auto* d3dDepth =
        dynamic_cast<D3D12Texture*>(&depth);

    if (d3dColor == nullptr ||
        d3dDepth == nullptr ||
        d3dColor->RenderTargetView().ptr == 0 ||
        d3dDepth->DepthStencilView().ptr == 0)
    {
        throw std::runtime_error(
            "Orbit D3D12 received incompatible color/depth render targets.");
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE colorHandle =
        d3dColor->RenderTargetView();

    const D3D12_CPU_DESCRIPTOR_HANDLE depthHandle =
        d3dDepth->DepthStencilView();

    nativeCommandList_->OMSetRenderTargets(
        1,
        &colorHandle,
        FALSE,
        &depthHandle);
}

void D3D12CommandList::SetViewport(
    const Viewport& viewport)
{
    const D3D12_VIEWPORT nativeViewport{
        viewport.x,
        viewport.y,
        viewport.width,
        viewport.height,
        viewport.minDepth,
        viewport.maxDepth
    };

    nativeCommandList_->RSSetViewports(
        1,
        &nativeViewport);
}

void D3D12CommandList::SetScissor(
    const ScissorRect& rect)
{
    const D3D12_RECT nativeRect{
        static_cast<LONG>(rect.left),
        static_cast<LONG>(rect.top),
        static_cast<LONG>(rect.right),
        static_cast<LONG>(rect.bottom)
    };

    nativeCommandList_->RSSetScissorRects(
        1,
        &nativeRect);
}

void D3D12CommandList::SetGraphicsPipeline(
    GraphicsPipeline& pipeline)
{
    auto* d3dPipeline =
        dynamic_cast<D3D12GraphicsPipeline*>(
            &pipeline);

    if (d3dPipeline == nullptr)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a graphics pipeline from another backend.");
    }

    nativeCommandList_->SetPipelineState(
        d3dPipeline->NativePipelineState());

    nativeCommandList_->SetGraphicsRootSignature(
        d3dPipeline->NativeRootSignature());

    nativeCommandList_->IASetPrimitiveTopology(
        d3dPipeline->NativeTopology());

    activePipeline_ = d3dPipeline;
}

void D3D12CommandList::SetGraphicsConstants(
    const std::span<const u32> dwords)
{
    if (activePipeline_ == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot bind graphics constants without an active pipeline.");
    }

    if (dwords.empty())
    {
        return;
    }

    if (dwords.size() >
        activePipeline_->PushConstantDwords())
    {
        throw std::runtime_error(
            "Orbit graphics constants exceed the active pipeline root constant range.");
    }

    nativeCommandList_->
        SetGraphicsRoot32BitConstants(
            0,
            static_cast<UINT>(
                dwords.size()),
            dwords.data(),
            0);
}

void D3D12CommandList::SetGraphicsBuffer(
    const u32 slot,
    Buffer& buffer)
{
    if (activePipeline_ == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot bind a graphics buffer without an active pipeline.");
    }

    auto* d3dBuffer =
        dynamic_cast<D3D12Buffer*>(
            &buffer);

    if (d3dBuffer == nullptr)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a shader buffer from another backend.");
    }

    const u32 rootParameter =
        activePipeline_->
            RootSrvParameterIndex(slot);

    nativeCommandList_->
        SetGraphicsRootShaderResourceView(
            rootParameter,
            d3dBuffer->Native()->
                GetGPUVirtualAddress());
}

void D3D12CommandList::SetVertexBuffer(
    Buffer& buffer,
    const u32 strideBytes)
{
    auto* d3dBuffer =
        dynamic_cast<D3D12Buffer*>(
            &buffer);

    if (d3dBuffer == nullptr)
    {
        throw std::runtime_error(
            "Orbit D3D12 received a vertex buffer from another backend.");
    }

    if (strideBytes == 0)
    {
        throw std::invalid_argument(
            "Orbit vertex buffer stride cannot be zero.");
    }

    D3D12_VERTEX_BUFFER_VIEW view{};
    view.BufferLocation =
        d3dBuffer->Native()->
            GetGPUVirtualAddress();
    view.SizeInBytes =
        static_cast<UINT>(
            d3dBuffer->SizeBytes());
    view.StrideInBytes =
        strideBytes;

    nativeCommandList_->IASetVertexBuffers(
        0,
        1,
        &view);
}

void D3D12CommandList::SetIndexBuffer(
    Buffer& buffer,
    const IndexFormat format)
{
    auto* d3dBuffer =
        dynamic_cast<D3D12Buffer*>(
            &buffer);

    if (d3dBuffer == nullptr)
    {
        throw std::runtime_error(
            "Orbit D3D12 received an index buffer from another backend.");
    }

    D3D12_INDEX_BUFFER_VIEW view{};
    view.BufferLocation =
        d3dBuffer->Native()->
            GetGPUVirtualAddress();
    view.SizeInBytes =
        static_cast<UINT>(
            d3dBuffer->SizeBytes());

    switch (format)
    {
    case IndexFormat::UInt16:
        view.Format = DXGI_FORMAT_R16_UINT;
        break;
    case IndexFormat::UInt32:
        view.Format = DXGI_FORMAT_R32_UINT;
        break;
    }

    nativeCommandList_->
        IASetIndexBuffer(&view);
}

void D3D12CommandList::DrawIndexed(
    const u32 indexCount,
    const u32 firstIndex,
    const i32 vertexOffset)
{
    nativeCommandList_->
        DrawIndexedInstanced(
            indexCount,
            1,
            firstIndex,
            vertexOffset,
            0);
}

void D3D12CommandList::Close()
{
    if (FAILED(nativeCommandList_->Close()))
    {
        throw std::runtime_error(
            "Orbit failed to close a D3D12 command list.");
    }
}

ID3D12GraphicsCommandList*
D3D12CommandList::Native() const noexcept
{
    return nativeCommandList_.Get();
}
} // namespace orbit::rhi::d3d12::detail
