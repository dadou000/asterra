#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/post_process/ToneMapping.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>

#include <memory>

namespace orbit::post_process
{
struct DisplayResolveSettings
{
    // View/presentation exposure. Physical/radiometric scene encoding happens
    // before this stage and must not depend on camera adaptation.
    f32 exposureScale{1.0F};
    ToneMappingConfig toneMapping{};
};

class DisplayResolveRenderer
{
public:
    DisplayResolveRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler);

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& sourceHdr,
        rhi::Texture& targetDisplayLinear,
        u32 width,
        u32 height,
        const DisplayResolveSettings& settings = {});

private:
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
};
} // namespace orbit::post_process
