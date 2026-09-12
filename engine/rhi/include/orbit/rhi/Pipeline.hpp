#pragma once

#include <orbit/core/Types.hpp>

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

struct GraphicsPipelineDesc
{
    ShaderBytecodeView vertexShader{};
    ShaderBytecodeView pixelShader{};
    std::span<const VertexAttribute> vertexAttributes{};
    u32 vertexStrideBytes{0};
    u32 pushConstantDwords{0};
    u32 shaderResourceBuffers{0};
    PrimitiveTopology topology{PrimitiveTopology::TriangleList};
    FillMode fillMode{FillMode::Solid};
    CullMode cullMode{CullMode::Back};
    bool depthTest{false};
    bool depthWrite{false};
};

class GraphicsPipeline
{
public:
    virtual ~GraphicsPipeline() = default;

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    [[nodiscard]] virtual u32 PushConstantDwords() const noexcept = 0;
    [[nodiscard]] virtual u32 ShaderResourceBuffers() const noexcept = 0;
    [[nodiscard]] virtual PrimitiveTopology Topology() const noexcept = 0;

protected:
    GraphicsPipeline() = default;
};
} // namespace orbit::rhi
