#include "D3D12Objects.hpp"

#include <d3d12.h>

#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orbit::rhi::d3d12::detail
{
namespace
{
[[nodiscard]] DXGI_FORMAT ToNativeVertexFormat(
    const VertexFormat format)
{
    switch (format)
    {
    case VertexFormat::Float2:
        return DXGI_FORMAT_R32G32_FLOAT;
    case VertexFormat::Float3:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case VertexFormat::Float4:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }

    throw std::invalid_argument(
        "Orbit received an invalid vertex format.");
}

[[nodiscard]] D3D12_FILL_MODE ToNativeFillMode(
    const FillMode mode)
{
    switch (mode)
    {
    case FillMode::Solid:
        return D3D12_FILL_MODE_SOLID;
    case FillMode::Wireframe:
        return D3D12_FILL_MODE_WIREFRAME;
    }

    throw std::invalid_argument(
        "Orbit received an invalid fill mode.");
}

[[nodiscard]] D3D12_CULL_MODE ToNativeCullMode(
    const CullMode mode)
{
    switch (mode)
    {
    case CullMode::None:
        return D3D12_CULL_MODE_NONE;
    case CullMode::Front:
        return D3D12_CULL_MODE_FRONT;
    case CullMode::Back:
        return D3D12_CULL_MODE_BACK;
    }

    throw std::invalid_argument(
        "Orbit received an invalid cull mode.");
}

[[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY_TYPE
ToNativeTopologyType(
    const PrimitiveTopology topology)
{
    switch (topology)
    {
    case PrimitiveTopology::TriangleList:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    case PrimitiveTopology::LineList:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    }

    throw std::invalid_argument(
        "Orbit received an invalid primitive topology.");
}

[[nodiscard]] D3D12_PRIMITIVE_TOPOLOGY
ToNativeTopology(
    const PrimitiveTopology topology)
{
    switch (topology)
    {
    case PrimitiveTopology::TriangleList:
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case PrimitiveTopology::LineList:
        return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }

    throw std::invalid_argument(
        "Orbit received an invalid primitive topology.");
}
} // namespace

D3D12GraphicsPipeline::D3D12GraphicsPipeline(
    ComPtr<ID3D12PipelineState> pipelineState,
    ComPtr<ID3D12RootSignature> rootSignature,
    const u32 pushConstantDwords,
    const u32 shaderResourceBuffers,
    const u32 rootSrvBaseParameter,
    const PrimitiveTopology topology)
    : pipelineState_(std::move(pipelineState)),
      rootSignature_(std::move(rootSignature)),
      pushConstantDwords_(pushConstantDwords),
      shaderResourceBuffers_(shaderResourceBuffers),
      rootSrvBaseParameter_(rootSrvBaseParameter),
      topology_(topology)
{
}

u32 D3D12GraphicsPipeline::PushConstantDwords() const noexcept
{
    return pushConstantDwords_;
}

u32 D3D12GraphicsPipeline::ShaderResourceBuffers() const noexcept
{
    return shaderResourceBuffers_;
}

PrimitiveTopology D3D12GraphicsPipeline::Topology() const noexcept
{
    return topology_;
}

ID3D12PipelineState*
D3D12GraphicsPipeline::NativePipelineState() const noexcept
{
    return pipelineState_.Get();
}

ID3D12RootSignature*
D3D12GraphicsPipeline::NativeRootSignature() const noexcept
{
    return rootSignature_.Get();
}

D3D12_PRIMITIVE_TOPOLOGY
D3D12GraphicsPipeline::NativeTopology() const noexcept
{
    return ToNativeTopology(topology_);
}

u32 D3D12GraphicsPipeline::RootSrvParameterIndex(
    const u32 slot) const
{
    if (slot >= shaderResourceBuffers_)
    {
        throw std::out_of_range(
            "Orbit graphics SRV slot exceeds the active pipeline layout.");
    }

    return rootSrvBaseParameter_ + slot;
}

std::unique_ptr<GraphicsPipeline>
D3D12Device::CreateGraphicsPipeline(
    const GraphicsPipelineDesc& desc)
{
    if (desc.vertexShader.data == nullptr ||
        desc.vertexShader.size == 0 ||
        desc.pixelShader.data == nullptr ||
        desc.pixelShader.size == 0)
    {
        throw std::invalid_argument(
            "Orbit graphics pipelines require vertex and pixel shader bytecode.");
    }

    const bool hasConstants =
        desc.pushConstantDwords > 0;

    const u32 rootSrvBase =
        hasConstants ? 1U : 0U;

    const u32 rootParameterCount =
        rootSrvBase +
        desc.shaderResourceBuffers;

    std::vector<D3D12_ROOT_PARAMETER>
        rootParameters(
            rootParameterCount);

    if (hasConstants)
    {
        auto& constants =
            rootParameters[0];

        constants.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        constants.Constants.ShaderRegister = 0;
        constants.Constants.RegisterSpace = 0;
        constants.Constants.Num32BitValues =
            desc.pushConstantDwords;
        constants.ShaderVisibility =
            D3D12_SHADER_VISIBILITY_ALL;
    }

    for (u32 slot = 0;
         slot < desc.shaderResourceBuffers;
         ++slot)
    {
        auto& parameter =
            rootParameters[
                rootSrvBase + slot];

        parameter.ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameter.Descriptor.ShaderRegister =
            slot;
        parameter.Descriptor.RegisterSpace = 0;
        parameter.ShaderVisibility =
            D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters =
        static_cast<UINT>(
            rootParameters.size());
    rootDesc.pParameters =
        rootParameters.empty()
            ? nullptr
            : rootParameters.data();
    rootDesc.NumStaticSamplers = 0;
    rootDesc.pStaticSamplers = nullptr;
    rootDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serializedRootSignature;
    ComPtr<ID3DBlob> rootSignatureError;

    if (FAILED(D3D12SerializeRootSignature(
            &rootDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &serializedRootSignature,
            &rootSignatureError)))
    {
        throw std::runtime_error(
            "Orbit failed to serialize a D3D12 root signature.");
    }

    ComPtr<ID3D12RootSignature> rootSignature;

    if (FAILED(nativeDevice_->CreateRootSignature(
            0,
            serializedRootSignature->
                GetBufferPointer(),
            serializedRootSignature->
                GetBufferSize(),
            IID_PPV_ARGS(&rootSignature))))
    {
        throw std::runtime_error(
            "Orbit failed to create a D3D12 root signature.");
    }

    std::vector<D3D12_INPUT_ELEMENT_DESC>
        inputElements;

    inputElements.reserve(
        desc.vertexAttributes.size());

    for (const VertexAttribute& attribute :
         desc.vertexAttributes)
    {
        D3D12_INPUT_ELEMENT_DESC element{};
        element.SemanticName = "TEXCOORD";
        element.SemanticIndex =
            attribute.location;
        element.Format =
            ToNativeVertexFormat(
                attribute.format);
        element.InputSlot = 0;
        element.AlignedByteOffset =
            attribute.offsetBytes;
        element.InputSlotClass =
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        element.InstanceDataStepRate = 0;

        inputElements.push_back(element);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC
        pipelineDesc{};

    pipelineDesc.pRootSignature =
        rootSignature.Get();

    pipelineDesc.VS = {
        desc.vertexShader.data,
        desc.vertexShader.size
    };

    pipelineDesc.PS = {
        desc.pixelShader.data,
        desc.pixelShader.size
    };

    pipelineDesc.BlendState.
        AlphaToCoverageEnable = FALSE;
    pipelineDesc.BlendState.
        IndependentBlendEnable = FALSE;

    auto& renderTargetBlend =
        pipelineDesc.BlendState.RenderTarget[0];

    renderTargetBlend.BlendEnable = FALSE;
    renderTargetBlend.LogicOpEnable = FALSE;
    renderTargetBlend.RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;

    pipelineDesc.SampleMask =
        std::numeric_limits<UINT>::max();

    pipelineDesc.RasterizerState.FillMode =
        ToNativeFillMode(desc.fillMode);
    pipelineDesc.RasterizerState.CullMode =
        ToNativeCullMode(desc.cullMode);
    pipelineDesc.RasterizerState.
        FrontCounterClockwise = FALSE;
    pipelineDesc.RasterizerState.DepthBias =
        D3D12_DEFAULT_DEPTH_BIAS;
    pipelineDesc.RasterizerState.
        DepthBiasClamp =
            D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pipelineDesc.RasterizerState.
        SlopeScaledDepthBias =
            D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pipelineDesc.RasterizerState.
        DepthClipEnable = TRUE;
    pipelineDesc.RasterizerState.
        MultisampleEnable = FALSE;
    pipelineDesc.RasterizerState.
        AntialiasedLineEnable = FALSE;
    pipelineDesc.RasterizerState.
        ForcedSampleCount = 0;
    pipelineDesc.RasterizerState.
        ConservativeRaster =
            D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    pipelineDesc.DepthStencilState.DepthEnable =
        desc.depthTest ? TRUE : FALSE;

    pipelineDesc.DepthStencilState.DepthWriteMask =
        desc.depthWrite
            ? D3D12_DEPTH_WRITE_MASK_ALL
            : D3D12_DEPTH_WRITE_MASK_ZERO;

    pipelineDesc.DepthStencilState.DepthFunc =
        D3D12_COMPARISON_FUNC_LESS_EQUAL;

    pipelineDesc.DepthStencilState.
        StencilEnable = FALSE;

    pipelineDesc.InputLayout = {
        inputElements.data(),
        static_cast<UINT>(
            inputElements.size())
    };

    pipelineDesc.PrimitiveTopologyType =
        ToNativeTopologyType(
            desc.topology);

    pipelineDesc.NumRenderTargets = 1;
    pipelineDesc.RTVFormats[0] =
        DXGI_FORMAT_R8G8B8A8_UNORM;

    pipelineDesc.DSVFormat =
        (desc.depthTest || desc.depthWrite)
            ? DXGI_FORMAT_D32_FLOAT
            : DXGI_FORMAT_UNKNOWN;

    pipelineDesc.SampleDesc.Count = 1;
    pipelineDesc.SampleDesc.Quality = 0;

    ComPtr<ID3D12PipelineState>
        pipelineState;

    if (FAILED(
            nativeDevice_->
                CreateGraphicsPipelineState(
                    &pipelineDesc,
                    IID_PPV_ARGS(
                        &pipelineState))))
    {
        throw std::runtime_error(
            "Orbit failed to create a D3D12 graphics pipeline.");
    }

    return std::make_unique<
        D3D12GraphicsPipeline>(
            std::move(pipelineState),
            std::move(rootSignature),
            desc.pushConstantDwords,
            desc.shaderResourceBuffers,
            rootSrvBase,
            desc.topology);
}
} // namespace orbit::rhi::d3d12::detail
