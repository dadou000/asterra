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
    HostVisible
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

protected:
    Texture() = default;
};
} // namespace orbit::rhi
