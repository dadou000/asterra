#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/rhi/Queue.hpp>
#include <orbit/rhi/Resource.hpp>

#include <span>

namespace orbit::rhi
{
struct ClearColor
{
    f32 red{0.0F};
    f32 green{0.0F};
    f32 blue{0.0F};
    f32 alpha{1.0F};
};

struct Viewport
{
    f32 x{0.0F};
    f32 y{0.0F};
    f32 width{0.0F};
    f32 height{0.0F};
    f32 minDepth{0.0F};
    f32 maxDepth{1.0F};
};

struct ScissorRect
{
    i32 left{0};
    i32 top{0};
    i32 right{0};
    i32 bottom{0};
};

enum class IndexFormat : u8
{
    UInt16,
    UInt32
};

class CommandAllocator
{
public:
    virtual ~CommandAllocator() = default;

    CommandAllocator(const CommandAllocator&) = delete;
    CommandAllocator& operator=(const CommandAllocator&) = delete;

    [[nodiscard]] virtual QueueType Type() const noexcept = 0;
    virtual void Reset() = 0;

protected:
    CommandAllocator() = default;
};

class CommandList
{
public:
    virtual ~CommandList() = default;

    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;

    [[nodiscard]] virtual QueueType Type() const noexcept = 0;

    virtual void Reset(CommandAllocator& allocator) = 0;

    virtual void Transition(
        Texture& texture,
        ResourceState before,
        ResourceState after) = 0;

    virtual void ClearColorTarget(
        Texture& texture,
        const ClearColor& color) = 0;

    virtual void SetRenderTarget(Texture& texture) = 0;
    virtual void SetViewport(const Viewport& viewport) = 0;
    virtual void SetScissor(const ScissorRect& rect) = 0;

    virtual void SetGraphicsPipeline(
        GraphicsPipeline& pipeline) = 0;

    virtual void SetGraphicsConstants(
        std::span<const u32> dwords) = 0;

    virtual void SetVertexBuffer(
        Buffer& buffer,
        u32 strideBytes) = 0;

    virtual void SetIndexBuffer(
        Buffer& buffer,
        IndexFormat format) = 0;

    virtual void DrawIndexed(
        u32 indexCount,
        u32 firstIndex = 0,
        i32 vertexOffset = 0) = 0;

    virtual void Close() = 0;

protected:
    CommandList() = default;
};
} // namespace orbit::rhi
