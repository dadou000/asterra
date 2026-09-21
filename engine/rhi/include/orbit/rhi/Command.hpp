#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/rhi/Query.hpp>
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

    virtual void Transition(
        Buffer& buffer,
        ResourceState before,
        ResourceState after) = 0;

    // Unlike Transition (which early-outs when before == after), this
    // always emits a barrier -- needed to express a same-state
    // UnorderedAccess write/write or write/read hazard between successive
    // passes of a ping-pong compute algorithm (e.g. depression-fill or
    // flow-accumulation relaxation), which Transition cannot express
    // since both sides of such a pass are ResourceState::UnorderedAccess.
    virtual void UavBarrier(Buffer& buffer) = 0;
    virtual void UavBarrier(Texture& texture) = 0;

    virtual void CopyBuffer(
        Buffer& source,
        u64 sourceOffsetBytes,
        Buffer& destination,
        u64 destinationOffsetBytes,
        u64 sizeBytes) = 0;

    // Copies tightly-packed RGBA8 (or the destination's own format)
    // pixel data from a HostVisible source buffer into the full extent
    // of destination. The caller transitions destination to
    // ResourceState::CopyDestination first and to ShaderResource
    // afterward, matching CopyBuffer's convention -- this call issues
    // no barrier of its own.
    virtual void CopyBufferToTexture(
        Buffer& source,
        u64 sourceOffsetBytes,
        Texture& destination) = 0;

    // Copies the complete texture into a tightly packed host/readback
    // buffer. The source must be in CopySource and the destination in
    // CopyDestination. RGBA8 is currently the supported readback format.
    virtual void CopyTextureToBuffer(
        Texture& source,
        Buffer& destination,
        u64 destinationOffsetBytes = 0) = 0;

    virtual void ClearColorTarget(
        Texture& texture,
        const ClearColor& color) = 0;

    virtual void ClearDepthTarget(
        Texture& texture,
        f32 depth) = 0;

    virtual void SetRenderTarget(Texture& texture) = 0;

    virtual void SetRenderTargets(
        Texture& color,
        Texture& depth) = 0;

    // General MRT entry point. All color targets must have equal extents.
    // depth may be null for color-only passes.
    virtual void SetRenderTargets(
        std::span<Texture* const> colors,
        Texture* depth) = 0;

    virtual void SetViewport(const Viewport& viewport) = 0;
    virtual void SetScissor(const ScissorRect& rect) = 0;

    virtual void SetGraphicsPipeline(
        GraphicsPipeline& pipeline) = 0;

    virtual void SetGraphicsConstants(
        std::span<const u32> dwords) = 0;

    virtual void SetGraphicsBuffer(
        u32 slot,
        Buffer& buffer) = 0;

    // Binds a sampled texture at a pipeline slot counted separately
    // from SetGraphicsBuffer's slots -- see
    // GraphicsPipelineDesc::sampledTextures. The texture must already
    // be in ResourceState::ShaderResource.
    virtual void SetGraphicsTexture(
        u32 slot,
        Texture& texture) = 0;

    virtual void SetComputePipeline(
        ComputePipeline& pipeline) = 0;

    virtual void SetComputeConstants(
        std::span<const u32> dwords) = 0;

    virtual void SetComputeBuffer(
        u32 slot,
        Buffer& buffer) = 0;

    // Binds a read/write storage image, distinct from SetComputeTexture's
    // read-only combined-image-sampler bind -- see
    // ComputePipelineDesc::storageTextures. The texture must already be
    // in ResourceState::UnorderedAccess.
    virtual void SetComputeStorageTexture(
        u32 slot,
        Texture& texture) = 0;

    // Read-only sampled texture, counted separately from and bound after
    // SetComputeBuffer/SetComputeStorageTexture's slots -- see
    // ComputePipelineDesc::sampledTextures. The texture must already be
    // in ResourceState::ShaderResource.
    virtual void SetComputeTexture(
        u32 slot,
        Texture& texture) = 0;

    virtual void Dispatch(
        u32 groupCountX,
        u32 groupCountY,
        u32 groupCountZ) = 0;

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

    // Non-indexed draw: SV_VertexID runs from firstVertex to
    // firstVertex + vertexCount. Used by GPU-driven meshes that
    // decode their own topology from the vertex index instead of
    // reading an index buffer -- see TerrainPreviewRenderer.
    virtual void Draw(
        u32 vertexCount,
        u32 firstVertex = 0) = 0;

    // Must be called for a query range before the first WriteTimestamp
    // into it each frame (and outside any active render target -- call
    // it right after Reset(), before the frame's first SetRenderTarget/
    // SetRenderTargets).
    virtual void ResetTimestampQueryPool(
        TimestampQueryPool& pool,
        u32 firstQuery,
        u32 count) = 0;

    // Records a GPU timestamp at this point in the command stream.
    // Safe to call both inside and outside an active render target.
    virtual void WriteTimestamp(
        TimestampQueryPool& pool,
        u32 query) = 0;

    virtual void Close() = 0;

protected:
    CommandList() = default;
};
} // namespace orbit::rhi
