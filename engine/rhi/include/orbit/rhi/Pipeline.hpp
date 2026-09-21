#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Resource.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace orbit::rhi
{
enum class VertexFormat : u8
{
    Float2,
    Float3,
    Float4
};

struct VertexAttribute
{
    u32 location{0};
    VertexFormat format{VertexFormat::Float3};
    u32 offsetBytes{0};
};

struct ShaderBytecodeView
{
    const u8* data{nullptr};
    std::size_t size{0};
};

enum class PrimitiveTopology : u8
{
    TriangleList,
    LineList
};

enum class FillMode : u8
{
    Solid,
    Wireframe
};

enum class CullMode : u8
{
    None,
    Front,
    Back
};

enum class BlendMode : u8
{
    Opaque,
    Alpha
};

enum class DepthCompare : u8
{
    LessEqual,
    GreaterEqual
};

struct GraphicsPipelineDesc
{
    ShaderBytecodeView vertexShader{};
    ShaderBytecodeView pixelShader{};
    std::span<const VertexAttribute> vertexAttributes{};
    u32 vertexStrideBytes{0};
    u32 pushConstantDwords{0};
    u32 shaderResourceBuffers{0};
    // Combined-image-sampler bindings, counted separately from and
    // placed after shaderResourceBuffers in the descriptor set layout
    // (binding indices shaderResourceBuffers .. shaderResourceBuffers
    // + sampledTextures - 1). See CommandList::SetGraphicsTexture.
    u32 sampledTextures{0};
    PrimitiveTopology topology{PrimitiveTopology::TriangleList};
    FillMode fillMode{FillMode::Solid};
    CullMode cullMode{CullMode::Back};
    BlendMode blendMode{BlendMode::Opaque};
    DepthCompare depthCompare{DepthCompare::LessEqual};
    bool depthTest{false};
    bool depthWrite{false};

    // Dynamic-rendering attachment contract. Pipelines must declare the
    // formats/count they are compatible with; this is also the MRT seam used
    // by the V0.0.7 lighting surface buffer.
    std::array<TextureFormat, 4> colorAttachmentFormats{
        TextureFormat::RGBA8_UNorm,
        TextureFormat::RGBA8_UNorm,
        TextureFormat::RGBA8_UNorm,
        TextureFormat::RGBA8_UNorm
    };
    u32 colorAttachmentCount{1U};
};

class GraphicsPipeline
{
public:
    virtual ~GraphicsPipeline() = default;

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    [[nodiscard]] virtual u32 PushConstantDwords() const noexcept = 0;
    [[nodiscard]] virtual u32 ShaderResourceBuffers() const noexcept = 0;
    [[nodiscard]] virtual u32 SampledTextures() const noexcept = 0;
    [[nodiscard]] virtual PrimitiveTopology Topology() const noexcept = 0;

protected:
    GraphicsPipeline() = default;
};

struct ComputePipelineDesc
{
    ShaderBytecodeView computeShader{};
    u32 pushConstantDwords{0};
    // Read/write storage buffer bindings, slots 0..shaderResourceBuffers-1.
    // See CommandList::SetComputeBuffer.
    u32 shaderResourceBuffers{0};
    // Read/write storage image bindings, counted separately from and
    // placed after shaderResourceBuffers -- mirrors GraphicsPipelineDesc::
    // sampledTextures' "counted separately, placed after" convention. See
    // CommandList::SetComputeStorageTexture.
    u32 storageTextures{0};
    // Read-only sampled texture bindings, placed after storageTextures.
    // See CommandList::SetComputeTexture.
    u32 sampledTextures{0};
};

class ComputePipeline
{
public:
    virtual ~ComputePipeline() = default;

    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    [[nodiscard]] virtual u32 PushConstantDwords() const noexcept = 0;
    [[nodiscard]] virtual u32 ShaderResourceBuffers() const noexcept = 0;
    [[nodiscard]] virtual u32 StorageTextures() const noexcept = 0;
    [[nodiscard]] virtual u32 SampledTextures() const noexcept = 0;

protected:
    ComputePipeline() = default;
};
} // namespace orbit::rhi
