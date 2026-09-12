#include "D3D12Objects.hpp"

#include <stdexcept>
#include <utility>

namespace orbit::rhi::d3d12::detail
{
D3D12Buffer::D3D12Buffer(
    ComPtr<ID3D12Resource> nativeResource,
    const BufferDesc desc)
    : nativeResource_(std::move(nativeResource)),
      desc_(desc)
{
}

u64 D3D12Buffer::SizeBytes() const noexcept
{
    return desc_.sizeBytes;
}

BufferUsage D3D12Buffer::Usage() const noexcept
{
    return desc_.usage;
}

MemoryUsage D3D12Buffer::Memory() const noexcept
{
    return desc_.memory;
}

std::byte* D3D12Buffer::Map()
{
    if (desc_.memory != MemoryUsage::HostVisible)
    {
        throw std::runtime_error(
            "Orbit cannot map a GPU-only D3D12 buffer.");
    }

    if (mapped_)
    {
        throw std::runtime_error(
            "Orbit D3D12 buffer is already mapped.");
    }

    D3D12_RANGE readRange{};
    readRange.Begin = 0;
    readRange.End = 0;

    void* data = nullptr;

    if (FAILED(nativeResource_->Map(
            0,
            &readRange,
            &data)))
    {
        throw std::runtime_error(
            "Orbit failed to map a D3D12 buffer.");
    }

    mapped_ = true;
    return static_cast<std::byte*>(data);
}

void D3D12Buffer::Unmap()
{
    if (!mapped_)
    {
        return;
    }

    nativeResource_->Unmap(0, nullptr);
    mapped_ = false;
}

ID3D12Resource* D3D12Buffer::Native() const noexcept
{
    return nativeResource_.Get();
}

std::unique_ptr<Buffer> D3D12Device::CreateBuffer(
    const BufferDesc& desc)
{
    if (desc.sizeBytes == 0)
    {
        throw std::invalid_argument(
            "Orbit cannot create a zero-sized buffer.");
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type =
        desc.memory == MemoryUsage::HostVisible
            ? D3D12_HEAP_TYPE_UPLOAD
            : D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty =
        D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference =
        D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = desc.sizeBytes;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const D3D12_RESOURCE_STATES initialState =
        desc.memory == MemoryUsage::HostVisible
            ? D3D12_RESOURCE_STATE_GENERIC_READ
            : ToNativeResourceState(desc.initialState);

    ComPtr<ID3D12Resource> resource;

    if (FAILED(nativeDevice_->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            initialState,
            nullptr,
            IID_PPV_ARGS(&resource))))
    {
        throw std::runtime_error(
            "Orbit failed to create a D3D12 buffer.");
    }

    return std::make_unique<D3D12Buffer>(
        std::move(resource),
        desc);
}
} // namespace orbit::rhi::d3d12::detail
