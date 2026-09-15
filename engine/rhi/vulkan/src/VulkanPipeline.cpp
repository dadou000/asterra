#include "VulkanObjects.hpp"

#include <array>
#include <stdexcept>
#include <vector>

namespace orbit::rhi::vulkan::detail
{
namespace
{
constexpr VkShaderStageFlags kAllGraphicsStages =
    VK_SHADER_STAGE_ALL_GRAPHICS;

[[nodiscard]] VkShaderModule CreateShaderModule(
    const VkDevice device,
    const ShaderBytecodeView& bytecode)
{
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = bytecode.size;
    createInfo.pCode =
        reinterpret_cast<const u32*>(bytecode.data);

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(
            device, &createInfo, nullptr, &module) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create a Vulkan shader module from "
            "SPIR-V bytecode.");
    }

    return module;
}
} // namespace

VkFormat ToNativeVertexFormat(const VertexFormat format)
{
    switch (format)
    {
    case VertexFormat::Float2:
        return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::Float3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::Float4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }

    throw std::invalid_argument(
        "Orbit received an invalid vertex format.");
}

VkCullModeFlags ToNativeCullMode(const CullMode mode)
{
    switch (mode)
    {
    case CullMode::None:
        return VK_CULL_MODE_NONE;
    case CullMode::Front:
        return VK_CULL_MODE_FRONT_BIT;
    case CullMode::Back:
        return VK_CULL_MODE_BACK_BIT;
    }

    throw std::invalid_argument(
        "Orbit received an invalid cull mode.");
}

VkPolygonMode ToNativeFillMode(const FillMode mode)
{
    switch (mode)
    {
    case FillMode::Solid:
        return VK_POLYGON_MODE_FILL;
    case FillMode::Wireframe:
        return VK_POLYGON_MODE_LINE;
    }

    throw std::invalid_argument(
        "Orbit received an invalid fill mode.");
}

VkCompareOp ToNativeDepthCompare(const DepthCompare compare)
{
    switch (compare)
    {
    case DepthCompare::LessEqual:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case DepthCompare::GreaterEqual:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    }

    throw std::invalid_argument(
        "Orbit received an invalid depth comparison mode.");
}

VkPrimitiveTopology ToNativeTopology(const PrimitiveTopology topology)
{
    switch (topology)
    {
    case PrimitiveTopology::TriangleList:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PrimitiveTopology::LineList:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    }

    throw std::invalid_argument(
        "Orbit received an invalid primitive topology.");
}

VulkanGraphicsPipeline::VulkanGraphicsPipeline(
    const VkDevice device,
    const VkPipeline pipeline,
    const VkPipelineLayout layout,
    const VkDescriptorSetLayout descriptorSetLayout,
    const u32 pushConstantDwords,
    const u32 shaderResourceBuffers,
    const u32 sampledTextures,
    const PrimitiveTopology topology)
    : device_(device),
      pipeline_(pipeline),
      layout_(layout),
      descriptorSetLayout_(descriptorSetLayout),
      pushConstantDwords_(pushConstantDwords),
      shaderResourceBuffers_(shaderResourceBuffers),
      sampledTextures_(sampledTextures),
      topology_(topology)
{
}

VulkanGraphicsPipeline::~VulkanGraphicsPipeline()
{
    if (pipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device_, pipeline_, nullptr);
    }

    if (layout_ != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    }

    if (descriptorSetLayout_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(
            device_, descriptorSetLayout_, nullptr);
    }
}

u32 VulkanGraphicsPipeline::PushConstantDwords() const noexcept
{
    return pushConstantDwords_;
}

u32 VulkanGraphicsPipeline::ShaderResourceBuffers() const noexcept
{
    return shaderResourceBuffers_;
}

u32 VulkanGraphicsPipeline::SampledTextures() const noexcept
{
    return sampledTextures_;
}

PrimitiveTopology VulkanGraphicsPipeline::Topology() const noexcept
{
    return topology_;
}

VkPipeline VulkanGraphicsPipeline::Native() const noexcept
{
    return pipeline_;
}

VkPipelineLayout VulkanGraphicsPipeline::Layout() const noexcept
{
    return layout_;
}

std::unique_ptr<GraphicsPipeline> VulkanDevice::CreateGraphicsPipeline(
    const GraphicsPipelineDesc& desc)
{
    if (desc.vertexShader.data == nullptr ||
        desc.vertexShader.size == 0 ||
        desc.pixelShader.data == nullptr ||
        desc.pixelShader.size == 0)
    {
        throw std::invalid_argument(
            "Orbit graphics pipelines require vertex and pixel "
            "shader bytecode.");
    }

    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayoutBinding> bindings(
        desc.shaderResourceBuffers + desc.sampledTextures);

    for (u32 slot = 0; slot < desc.shaderResourceBuffers; ++slot)
    {
        auto& binding = bindings[slot];
        binding.binding = slot;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = kAllGraphicsStages;
    }

    // Texture bindings sit right after the buffer bindings -- see
    // CommandList::SetGraphicsTexture, which binds at
    // shaderResourceBuffers + slot.
    for (u32 slot = 0; slot < desc.sampledTextures; ++slot)
    {
        auto& binding = bindings[desc.shaderResourceBuffers + slot];
        binding.binding = desc.shaderResourceBuffers + slot;
        binding.descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = kAllGraphicsStages;
    }

    if (!bindings.empty())
    {
        VkDescriptorSetLayoutCreateInfo layoutCreateInfo{};
        layoutCreateInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        // Bound per-draw via vkCmdPushDescriptorSetKHR instead of a
        // real VkDescriptorPool/VkDescriptorSet -- the Vulkan
        // analogue of the D3D12 root-descriptor SRV binding every
        // buffer in this codebase uses today (see SetGraphicsBuffer
        // in VulkanCommands.cpp).
        layoutCreateInfo.flags =
            VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        layoutCreateInfo.bindingCount =
            static_cast<u32>(bindings.size());
        layoutCreateInfo.pBindings = bindings.data();

        if (vkCreateDescriptorSetLayout(
                nativeDevice_,
                &layoutCreateInfo,
                nullptr,
                &descriptorSetLayout) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Orbit failed to create a Vulkan push-descriptor set "
                "layout.");
        }
    }

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = kAllGraphicsStages;
    pushConstantRange.offset = 0;
    pushConstantRange.size = desc.pushConstantDwords * sizeof(u32);

    const bool hasConstants = desc.pushConstantDwords > 0;

    VkPipelineLayoutCreateInfo layoutCreateInfo{};
    layoutCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

    if (descriptorSetLayout != VK_NULL_HANDLE)
    {
        layoutCreateInfo.setLayoutCount = 1;
        layoutCreateInfo.pSetLayouts = &descriptorSetLayout;
    }

    if (hasConstants)
    {
        layoutCreateInfo.pushConstantRangeCount = 1;
        layoutCreateInfo.pPushConstantRanges = &pushConstantRange;
    }

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(
            nativeDevice_,
            &layoutCreateInfo,
            nullptr,
            &pipelineLayout) != VK_SUCCESS)
    {
        if (descriptorSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(
                nativeDevice_, descriptorSetLayout, nullptr);
        }

        throw std::runtime_error(
            "Orbit failed to create a Vulkan pipeline layout.");
    }

    const VkShaderModule vertexModule =
        CreateShaderModule(nativeDevice_, desc.vertexShader);
    const VkShaderModule pixelModule =
        CreateShaderModule(nativeDevice_, desc.pixelShader);

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {
            .sType =
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertexModule,
            .pName = "main"
        },
        {
            .sType =
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = pixelModule,
            .pName = "main"
        }
    }};

    VkVertexInputBindingDescription vertexBinding{};
    vertexBinding.binding = 0;
    vertexBinding.stride = desc.vertexStrideBytes;
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> vertexAttributes;
    vertexAttributes.reserve(desc.vertexAttributes.size());

    for (const VertexAttribute& attribute : desc.vertexAttributes)
    {
        VkVertexInputAttributeDescription nativeAttribute{};
        nativeAttribute.location = attribute.location;
        nativeAttribute.binding = 0;
        nativeAttribute.format = ToNativeVertexFormat(attribute.format);
        nativeAttribute.offset = attribute.offsetBytes;

        vertexAttributes.push_back(nativeAttribute);
    }

    VkPipelineVertexInputStateCreateInfo vertexInputState{};
    vertexInputState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    if (!vertexAttributes.empty())
    {
        vertexInputState.vertexBindingDescriptionCount = 1;
        vertexInputState.pVertexBindingDescriptions = &vertexBinding;
        vertexInputState.vertexAttributeDescriptionCount =
            static_cast<u32>(vertexAttributes.size());
        vertexInputState.pVertexAttributeDescriptions =
            vertexAttributes.data();
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{};
    inputAssemblyState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssemblyState.topology = ToNativeTopology(desc.topology);

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizationState{};
    rasterizationState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizationState.polygonMode = ToNativeFillMode(desc.fillMode);
    rasterizationState.cullMode = ToNativeCullMode(desc.cullMode);
    // Matches the D3D12 backend's FrontCounterClockwise = FALSE.
    rasterizationState.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizationState.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo multisampleState{};
    multisampleState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampleState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencilState{};
    depthStencilState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencilState.depthTestEnable =
        desc.depthTest ? VK_TRUE : VK_FALSE;
    depthStencilState.depthWriteEnable =
        desc.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencilState.depthCompareOp =
        ToNativeDepthCompare(desc.depthCompare);

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable = VK_FALSE;
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlendState{};
    colorBlendState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlendState.attachmentCount = 1;
    colorBlendState.pAttachments = &colorBlendAttachment;

    const std::array dynamicStates{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount =
        static_cast<u32>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Every pipeline in this codebase always renders to one color
    // target; a depth target is only attached when the pipeline
    // actually tests/writes depth -- mirrors the D3D12 backend's
    // DSVFormat = (depthTest || depthWrite) ? D32_FLOAT : UNKNOWN.
    constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    const bool usesDepth = desc.depthTest || desc.depthWrite;

    VkPipelineRenderingCreateInfo renderingCreateInfo{};
    renderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingCreateInfo.colorAttachmentCount = 1;
    renderingCreateInfo.pColorAttachmentFormats = &kColorFormat;
    renderingCreateInfo.depthAttachmentFormat =
        usesDepth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo pipelineCreateInfo{};
    pipelineCreateInfo.sType =
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCreateInfo.pNext = &renderingCreateInfo;
    pipelineCreateInfo.stageCount =
        static_cast<u32>(stages.size());
    pipelineCreateInfo.pStages = stages.data();
    pipelineCreateInfo.pVertexInputState = &vertexInputState;
    pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
    pipelineCreateInfo.pViewportState = &viewportState;
    pipelineCreateInfo.pRasterizationState = &rasterizationState;
    pipelineCreateInfo.pMultisampleState = &multisampleState;
    pipelineCreateInfo.pDepthStencilState = &depthStencilState;
    pipelineCreateInfo.pColorBlendState = &colorBlendState;
    pipelineCreateInfo.pDynamicState = &dynamicState;
    pipelineCreateInfo.layout = pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateGraphicsPipelines(
        nativeDevice_,
        VK_NULL_HANDLE,
        1,
        &pipelineCreateInfo,
        nullptr,
        &pipeline);

    vkDestroyShaderModule(nativeDevice_, vertexModule, nullptr);
    vkDestroyShaderModule(nativeDevice_, pixelModule, nullptr);

    if (result != VK_SUCCESS)
    {
        vkDestroyPipelineLayout(nativeDevice_, pipelineLayout, nullptr);

        if (descriptorSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(
                nativeDevice_, descriptorSetLayout, nullptr);
        }

        throw std::runtime_error(
            "Orbit failed to create a Vulkan graphics pipeline.");
    }

    return std::make_unique<VulkanGraphicsPipeline>(
        nativeDevice_,
        pipeline,
        pipelineLayout,
        descriptorSetLayout,
        desc.pushConstantDwords,
        desc.shaderResourceBuffers,
        desc.sampledTextures,
        desc.topology);
}

VulkanComputePipeline::VulkanComputePipeline(
    const VkDevice device,
    const VkPipeline pipeline,
    const VkPipelineLayout layout,
    const VkDescriptorSetLayout descriptorSetLayout,
    const u32 pushConstantDwords,
    const u32 shaderResourceBuffers,
    const u32 storageTextures,
    const u32 sampledTextures)
    : device_(device),
      pipeline_(pipeline),
      layout_(layout),
      descriptorSetLayout_(descriptorSetLayout),
      pushConstantDwords_(pushConstantDwords),
      shaderResourceBuffers_(shaderResourceBuffers),
      storageTextures_(storageTextures),
      sampledTextures_(sampledTextures)
{
}

VulkanComputePipeline::~VulkanComputePipeline()
{
    if (pipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device_, pipeline_, nullptr);
    }

    if (layout_ != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
    }

    if (descriptorSetLayout_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(
            device_, descriptorSetLayout_, nullptr);
    }
}

u32 VulkanComputePipeline::PushConstantDwords() const noexcept
{
    return pushConstantDwords_;
}

u32 VulkanComputePipeline::ShaderResourceBuffers() const noexcept
{
    return shaderResourceBuffers_;
}

u32 VulkanComputePipeline::StorageTextures() const noexcept
{
    return storageTextures_;
}

u32 VulkanComputePipeline::SampledTextures() const noexcept
{
    return sampledTextures_;
}

VkPipeline VulkanComputePipeline::Native() const noexcept
{
    return pipeline_;
}

VkPipelineLayout VulkanComputePipeline::Layout() const noexcept
{
    return layout_;
}

std::unique_ptr<ComputePipeline> VulkanDevice::CreateComputePipeline(
    const ComputePipelineDesc& desc)
{
    if (desc.computeShader.data == nullptr ||
        desc.computeShader.size == 0)
    {
        throw std::invalid_argument(
            "Orbit compute pipelines require compute shader bytecode.");
    }

    const u32 storageTextureBase = desc.shaderResourceBuffers;
    const u32 sampledTextureBase =
        storageTextureBase + desc.storageTextures;

    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayoutBinding> bindings(
        desc.shaderResourceBuffers +
        desc.storageTextures +
        desc.sampledTextures);

    for (u32 slot = 0; slot < desc.shaderResourceBuffers; ++slot)
    {
        auto& binding = bindings[slot];
        binding.binding = slot;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    // Storage (read/write) image bindings sit right after the buffer
    // bindings -- see CommandList::SetComputeStorageTexture.
    for (u32 slot = 0; slot < desc.storageTextures; ++slot)
    {
        auto& binding = bindings[storageTextureBase + slot];
        binding.binding = storageTextureBase + slot;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    // Read-only sampled textures sit after the storage images -- see
    // CommandList::SetComputeTexture.
    for (u32 slot = 0; slot < desc.sampledTextures; ++slot)
    {
        auto& binding = bindings[sampledTextureBase + slot];
        binding.binding = sampledTextureBase + slot;
        binding.descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    if (!bindings.empty())
    {
        VkDescriptorSetLayoutCreateInfo layoutCreateInfo{};
        layoutCreateInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        // Same push-descriptor binding model as the graphics pipeline
        // layout above -- see its comment for why.
        layoutCreateInfo.flags =
            VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        layoutCreateInfo.bindingCount =
            static_cast<u32>(bindings.size());
        layoutCreateInfo.pBindings = bindings.data();

        if (vkCreateDescriptorSetLayout(
                nativeDevice_,
                &layoutCreateInfo,
                nullptr,
                &descriptorSetLayout) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Orbit failed to create a Vulkan compute push-descriptor "
                "set layout.");
        }
    }

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = desc.pushConstantDwords * sizeof(u32);

    const bool hasConstants = desc.pushConstantDwords > 0;

    VkPipelineLayoutCreateInfo layoutCreateInfo{};
    layoutCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

    if (descriptorSetLayout != VK_NULL_HANDLE)
    {
        layoutCreateInfo.setLayoutCount = 1;
        layoutCreateInfo.pSetLayouts = &descriptorSetLayout;
    }

    if (hasConstants)
    {
        layoutCreateInfo.pushConstantRangeCount = 1;
        layoutCreateInfo.pPushConstantRanges = &pushConstantRange;
    }

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(
            nativeDevice_,
            &layoutCreateInfo,
            nullptr,
            &pipelineLayout) != VK_SUCCESS)
    {
        if (descriptorSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(
                nativeDevice_, descriptorSetLayout, nullptr);
        }

        throw std::runtime_error(
            "Orbit failed to create a Vulkan compute pipeline layout.");
    }

    const VkShaderModule computeModule =
        CreateShaderModule(nativeDevice_, desc.computeShader);

    VkComputePipelineCreateInfo pipelineCreateInfo{};
    pipelineCreateInfo.sType =
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineCreateInfo.stage.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineCreateInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineCreateInfo.stage.module = computeModule;
    pipelineCreateInfo.stage.pName = "main";
    pipelineCreateInfo.layout = pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = vkCreateComputePipelines(
        nativeDevice_,
        VK_NULL_HANDLE,
        1,
        &pipelineCreateInfo,
        nullptr,
        &pipeline);

    vkDestroyShaderModule(nativeDevice_, computeModule, nullptr);

    if (result != VK_SUCCESS)
    {
        vkDestroyPipelineLayout(nativeDevice_, pipelineLayout, nullptr);

        if (descriptorSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(
                nativeDevice_, descriptorSetLayout, nullptr);
        }

        throw std::runtime_error(
            "Orbit failed to create a Vulkan compute pipeline.");
    }

    return std::make_unique<VulkanComputePipeline>(
        nativeDevice_,
        pipeline,
        pipelineLayout,
        descriptorSetLayout,
        desc.pushConstantDwords,
        desc.shaderResourceBuffers,
        desc.storageTextures,
        desc.sampledTextures);
}
} // namespace orbit::rhi::vulkan::detail
