#include "D3D12Objects.hpp"

#include <stdexcept>
#include <utility>

namespace orbit::rhi::d3d12::detail
{
namespace
{
[[nodiscard]] DXGI_FORMAT ToNativeTextureFormat(
    const TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::RGBA8_UNorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::D32_Float:
        return DXGI_FORMAT_D32_FLOAT;
    }

    throw std::invalid_argument(
        "Orbit received an invalid texture format.");
}
} // namespace

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

D3D12Texture::D3D12Texture(
    ComPtr<ID3D12Resource> nativeResource,
    const u32 width,
    const u32 height,
    const TextureFormat format,
    const D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView,
    const D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView,
    ComPtr<ID3D12DescriptorHeap> ownedDescriptorHeap)
    : nativeResource_(std::move(nativeResource)),
      ownedDescriptorHeap_(std::move(ownedDescriptorHeap)),
      width_(width),
      height_(height),
      format_(format),
      renderTargetView_(renderTargetView),
      depthStencilView_(depthStencilView)
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

TextureFormat D3D12Texture::Format() const noexcept
{
    return format_;
}

ID3D12Resource* D3D12Texture::Native() const noexcept
{
    return nativeResource_.Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE
D3D12Texture::RenderTargetView() const noexcept
{
    return renderTargetView_;
}

D3D12_CPU_DESCRIPTOR_HANDLE
D3D12Texture::DepthStencilView() const noexcept
{
    return depthStencilView_;
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
    resourceDesc.Dimension =
        D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = desc.sizeBytes;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout =
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const D3D12_RESOURCE_STATES initialState =
        desc.memory == MemoryUsage::HostVisible
            ? D3D12_RESOURCE_STATE_GENERIC_READ
            : ToNativeResourceState(
                desc.initialState);

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

std::unique_ptr<Texture> D3D12Device::CreateTexture(
    const TextureDesc& desc)
{
    if (desc.width == 0 ||
        desc.height == 0)
    {
        throw std::invalid_argument(
            "Orbit cannot create a zero-sized texture.");
    }

    const DXGI_FORMAT nativeFormat =
        ToNativeTextureFormat(desc.format);

    D3D12_RESOURCE_FLAGS flags =
        D3D12_RESOURCE_FLAG_NONE;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = nativeFormat;

    D3D12_DESCRIPTOR_HEAP_TYPE heapType{};

    switch (desc.format)
    {
    case TextureFormat::RGBA8_UNorm:
        flags =
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        clearValue.Color[0] = 0.0F;
        clearValue.Color[1] = 0.0F;
        clearValue.Color[2] = 0.0F;
        clearValue.Color[3] = 1.0F;
        heapType =
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        break;

    case TextureFormat::D32_Float:
        flags =
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        clearValue.DepthStencil.Depth = 1.0F;
        clearValue.DepthStencil.Stencil = 0;
        heapType =
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        break;
    }

    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type =
        D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty =
        D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference =
        D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension =
        D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = desc.width;
    resourceDesc.Height = desc.height;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = nativeFormat;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout =
        D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = flags;

    ComPtr<ID3D12Resource> resource;

    if (FAILED(nativeDevice_->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            ToNativeResourceState(
                desc.initialState),
            &clearValue,
            IID_PPV_ARGS(&resource))))
    {
        throw std::runtime_error(
            "Orbit failed to create a D3D12 texture.");
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = heapType;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags =
        D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    ComPtr<ID3D12DescriptorHeap> descriptorHeap;

    if (FAILED(nativeDevice_->CreateDescriptorHeap(
            &heapDesc,
            IID_PPV_ARGS(&descriptorHeap))))
    {
        throw std::runtime_error(
            "Orbit failed to create a D3D12 texture descriptor heap.");
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE handle =
        descriptorHeap->
            GetCPUDescriptorHandleForHeapStart();

    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};

    switch (desc.format)
    {
    case TextureFormat::RGBA8_UNorm:
        nativeDevice_->CreateRenderTargetView(
            resource.Get(),
            nullptr,
            handle);
        rtv = handle;
        break;

    case TextureFormat::D32_Float:
        nativeDevice_->CreateDepthStencilView(
            resource.Get(),
            nullptr,
            handle);
        dsv = handle;
        break;
    }

    return std::make_unique<D3D12Texture>(
        std::move(resource),
        desc.width,
        desc.height,
        desc.format,
        rtv,
        dsv,
        std::move(descriptorHeap));
}
} // namespace orbit::rhi::d3d12::detail
