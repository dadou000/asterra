#pragma once

#include <orbit/core/Types.hpp>

#include <cstddef>

namespace orbit::rhi
{
enum class ResourceState : u8
{
    Common,
    Present,
    RenderTarget,
    DepthWrite,
    DepthRead,
    VertexOrConstantBuffer,
    IndexBuffer,
    ShaderResource,
    UnorderedAccess,
    CopySource,
    CopyDestination
};

enum class MemoryUsage : u8
{
    GpuOnly,
    HostVisible,
    // CPU-readable staging memory for GPU -> host transfers. Mapping a
    // readback buffer invalidates non-coherent Vulkan memory before the
    // pointer is returned.
    HostReadback
};

enum class BufferUsage : u8
{
    Generic,
    Vertex,
    Index,
    Constant,
    Structured
};

struct BufferDesc
{
    u64 sizeBytes{0};
    BufferUsage usage{BufferUsage::Generic};
    MemoryUsage memory{MemoryUsage::GpuOnly};
    ResourceState initialState{ResourceState::Common};
};

enum class TextureFormat : u8
{
    RGBA8_UNorm,
    D32_Float,
    // Single/dual-channel float rasters for compute-generated data
    // (elevation, flow accumulation, slope pairs, etc.) -- meters-scale
    // values need more range/precision than RGBA8_UNorm's [0,1] can hold.
    R16_Float,
    RG16_Float,
    RGBA16_Float,
    R16_UInt,
    R32_Float,
    RG32_Float
};

[[nodiscard]] constexpr u32 TextureFormatBytesPerTexel(
    const TextureFormat format) noexcept
{
    switch (format)
    {
    case TextureFormat::RGBA8_UNorm:
    case TextureFormat::D32_Float:
    case TextureFormat::R32_Float:
        return 4U;
    case TextureFormat::R16_Float:
    case TextureFormat::R16_UInt:
        return 2U;
    case TextureFormat::RG16_Float:
        return 4U;
    case TextureFormat::RGBA16_Float:
    case TextureFormat::RG32_Float:
        return 8U;
    }

    return 0U;
}

struct TextureDesc
{
    u32 width{0};
    u32 height{0};
    TextureFormat format{TextureFormat::RGBA8_UNorm};
    ResourceState initialState{ResourceState::Common};
    // Opt-in (not unconditional, unlike the backend's blanket
    // TRANSFER_DST/SAMPLED usage flags): allows a compute shader to
    // imageLoad/imageStore this texture. Storage-image support isn't
    // universally free on every format/tiling combination, and it
    // signals real UAV intent, so callers that don't need it shouldn't
    // pay for it.
    bool allowUnorderedAccess{false};
};

class Buffer
{
public:
    virtual ~Buffer() = default;

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    [[nodiscard]] virtual u64 SizeBytes() const noexcept = 0;
    [[nodiscard]] virtual BufferUsage Usage() const noexcept = 0;
    [[nodiscard]] virtual MemoryUsage Memory() const noexcept = 0;

    [[nodiscard]] virtual std::byte* Map() = 0;
    virtual void Unmap() = 0;

protected:
    Buffer() = default;
};

class Texture
{
public:
    virtual ~Texture() = default;

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    [[nodiscard]] virtual u32 Width() const noexcept = 0;
    [[nodiscard]] virtual u32 Height() const noexcept = 0;
    [[nodiscard]] virtual TextureFormat Format() const noexcept = 0;

protected:
    Texture() = default;
};
} // namespace orbit::rhi
