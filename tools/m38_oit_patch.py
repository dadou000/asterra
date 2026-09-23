from pathlib import Path

def rep(path, old, new):
    p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
    if c!=1: raise RuntimeError(f'{path}: expected 1 match got {c}: {old[:120]!r}')
    p.write_text(t.replace(old,new,1),encoding='utf-8')

# RHI additive blending.
p='engine/rhi/include/orbit/rhi/Pipeline.hpp'
rep(p,'enum class BlendMode : u8\n{\n    Opaque,\n    Alpha\n};','enum class BlendMode : u8\n{\n    Opaque,\n    Alpha,\n    Additive\n};')

p='engine/rhi/vulkan/src/VulkanPipeline.cpp'
rep(p,
'''        case BlendMode::Alpha:\n            colorBlendAttachment.blendEnable = VK_TRUE;\n            colorBlendAttachment.srcColorBlendFactor =\n                VK_BLEND_FACTOR_SRC_ALPHA;\n            colorBlendAttachment.dstColorBlendFactor =\n                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;\n            colorBlendAttachment.colorBlendOp =\n                VK_BLEND_OP_ADD;\n            colorBlendAttachment.srcAlphaBlendFactor =\n                VK_BLEND_FACTOR_ONE;\n            colorBlendAttachment.dstAlphaBlendFactor =\n                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;\n            colorBlendAttachment.alphaBlendOp =\n                VK_BLEND_OP_ADD;\n            break;\n''',
'''        case BlendMode::Alpha:\n            colorBlendAttachment.blendEnable = VK_TRUE;\n            colorBlendAttachment.srcColorBlendFactor =\n                VK_BLEND_FACTOR_SRC_ALPHA;\n            colorBlendAttachment.dstColorBlendFactor =\n                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;\n            colorBlendAttachment.colorBlendOp =\n                VK_BLEND_OP_ADD;\n            colorBlendAttachment.srcAlphaBlendFactor =\n                VK_BLEND_FACTOR_ONE;\n            colorBlendAttachment.dstAlphaBlendFactor =\n                VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;\n            colorBlendAttachment.alphaBlendOp =\n                VK_BLEND_OP_ADD;\n            break;\n\n        case BlendMode::Additive:\n            colorBlendAttachment.blendEnable = VK_TRUE;\n            colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;\n            colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;\n            colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;\n            colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;\n            colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;\n            colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;\n            break;\n''')

# Renderer header: per-size/per-frame OIT targets.
p='engine/volume_render/include/orbit/volume_render/VolumeParticleRenderer.hpp'
rep(p,'#include <memory>\n#include <span>\n#include <vector>','#include <map>\n#include <memory>\n#include <span>\n#include <vector>')
rep(p,
'''        const render_view::CameraState& camera,\n        math::Double3 cameraPositionRelativeToPresentationOriginMeters,\n        f32 radiusPixels = 3.0F);''',
'''        const render_view::CameraState& camera,\n        math::Double3 cameraPositionRelativeToPresentationOriginMeters,\n        u32 frameIndex,\n        f32 radiusPixels = 3.0F);''')
rep(p,
'''private:\n    VolumeParticleGpuState state_;\n    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;\n    std::unique_ptr<rhi::GraphicsPipeline> splashPipeline_;\n    std::unique_ptr<rhi::GraphicsPipeline> dropletPipeline_;\n    std::vector<VolumeParticleTerrainCollisionPage> terrainCollisionPages_;''',
'''private:\n    struct OitTargets\n    {\n        std::unique_ptr<rhi::Texture> accumulation;\n        std::unique_ptr<rhi::Texture> opticalDepth;\n        rhi::ResourceState accumulationState{rhi::ResourceState::ShaderResource};\n        rhi::ResourceState opticalDepthState{rhi::ResourceState::ShaderResource};\n    };\n\n    [[nodiscard]] OitTargets& EnsureOitTargets(u32 width, u32 height, u32 frameIndex);\n\n    rhi::Device* device_{nullptr};\n    u32 framesInFlight_{1U};\n    VolumeParticleGpuState state_;\n    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;\n    std::unique_ptr<rhi::GraphicsPipeline> splashPipeline_;\n    std::unique_ptr<rhi::GraphicsPipeline> dropletPipeline_;\n    std::unique_ptr<rhi::GraphicsPipeline> oitCompositePipeline_;\n    std::map<u64, std::vector<OitTargets>> oitTargets_;\n    std::vector<VolumeParticleTerrainCollisionPage> terrainCollisionPages_;''')

# Renderer source shader conversions + composite.
p='engine/volume_render/src/VolumeParticleRenderer.cpp'
# Particle pixel output.
rep(p,
'''float4 main(VSOutput input) : SV_Target0\n{\n    const float radius2 = dot(input.uv, input.uv);''',
'''struct OitOutput { float4 accumulation : SV_Target0; float4 opticalDepth : SV_Target1; };\nOitOutput main(VSOutput input)\n{\n    const float radius2 = dot(input.uv, input.uv);''')
rep(p,
'''    return float4(color, alpha);\n}\n)";''',
'''    OitOutput output;\n    const float opticalDepth = -log(max(1.0 - saturate(alpha), 1.0e-4));\n    output.accumulation = float4(color * alpha, alpha);\n    output.opticalDepth = float4(opticalDepth, 0.0, 0.0, 0.0);\n    return output;\n}\n)";''')
# Splash pixel.
rep(p,
'''float4 main(VSOutput i):SV_Target0 {\n    float r=length(i.uv); if(r>=1.0||r<0.42) discard;\n    float ring=(1.0-smoothstep(0.42,0.62,r))*smoothstep(0.42,0.52,r);\n    float impact=saturate(i.impact/8.0); float alpha=ring*(0.35+0.55*impact);\n    float3 foam=lerp(float3(0.72,0.84,0.90),float3(1.0,1.0,1.0),impact)*i.tint;\n    return float4(foam,alpha);\n}\n)";''',
'''struct OitOutput { float4 accumulation:SV_Target0; float4 opticalDepth:SV_Target1; };\nOitOutput main(VSOutput i) {\n    float r=length(i.uv); if(r>=1.0||r<0.42) discard;\n    float ring=(1.0-smoothstep(0.42,0.62,r))*smoothstep(0.42,0.52,r);\n    float impact=saturate(i.impact/8.0); float alpha=ring*(0.35+0.55*impact);\n    float3 foam=lerp(float3(0.72,0.84,0.90),float3(1.0,1.0,1.0),impact)*i.tint;\n    OitOutput o; float optical=-log(max(1.0-saturate(alpha),1.0e-4));\n    o.accumulation=float4(foam*alpha,alpha); o.opticalDepth=float4(optical,0,0,0); return o;\n}\n)";''')
# Droplet pixel.
rep(p,
'''float4 main(VSOutput i):SV_Target0 { float r2=dot(i.uv,i.uv); if(r2>=1.0||i.life<=0.0) discard; float alpha=(1.0-smoothstep(0.2,1.0,r2))*i.life*0.82; float3 c=lerp(float3(0.70,0.84,0.94),float3(1,1,1),0.65)*i.tint; return float4(c,alpha); }\n)";''',
'''struct OitOutput { float4 accumulation:SV_Target0; float4 opticalDepth:SV_Target1; };\nOitOutput main(VSOutput i) { float r2=dot(i.uv,i.uv); if(r2>=1.0||i.life<=0.0) discard; float alpha=(1.0-smoothstep(0.2,1.0,r2))*i.life*0.82; float3 c=lerp(float3(0.70,0.84,0.94),float3(1,1,1),0.65)*i.tint; OitOutput o; float optical=-log(max(1.0-saturate(alpha),1.0e-4)); o.accumulation=float4(c*alpha,alpha); o.opticalDepth=float4(optical,0,0,0); return o; }\n)";''')

# Insert fullscreen composite shaders before namespace closes after pixel shader.
marker='''    return output;\n}\n)";\n}\n\nVolumeParticleRenderer::VolumeParticleRenderer('''
composite='''    return output;\n}\n)";\n\nconstexpr const char* kOitCompositeVertexShader = R"(\nstruct O { float4 position:SV_Position; float2 uv:TEXCOORD0; };\nO main(uint id:SV_VertexID){ static const float2 p[6]={float2(-1,-1),float2(-1,1),float2(1,-1),float2(1,-1),float2(-1,1),float2(1,1)}; static const float2 u[6]={float2(0,1),float2(0,0),float2(1,1),float2(1,1),float2(0,0),float2(1,0)}; O o;o.position=float4(p[id],0,1);o.uv=u[id];return o; }\n)";\nconstexpr const char* kOitCompositePixelShader = R"(\nstruct O { float4 position:SV_Position; float2 uv:TEXCOORD0; };\n[[vk::binding(0,0)]][[vk::combinedImageSampler]] Texture2D g_accum;\n[[vk::binding(0,0)]][[vk::combinedImageSampler]] SamplerState g_accumSampler;\n[[vk::binding(1,0)]][[vk::combinedImageSampler]] Texture2D g_optical;\n[[vk::binding(1,0)]][[vk::combinedImageSampler]] SamplerState g_opticalSampler;\nfloat4 main(O i):SV_Target0 { float4 a=g_accum.Sample(g_accumSampler,i.uv); float optical=max(g_optical.Sample(g_opticalSampler,i.uv).r,0.0); if(a.a<=1.0e-6||optical<=1.0e-6) discard; float3 color=a.rgb/max(a.a,1.0e-6); float alpha=1.0-exp(-min(optical,20.0)); return float4(color,alpha); }\n)";\n}\n\nVolumeParticleRenderer::VolumeParticleRenderer('''
rep(p,marker,composite)

# Constructor stores device/frame count.
rep(p,
'''    const u32 framesInFlight)\n    : state_(device, compiler, framesInFlight)\n{''',
'''    const u32 framesInFlight)\n    : device_(&device),\n      framesInFlight_(framesInFlight),\n      state_(device, compiler, framesInFlight)\n{''')
# Compile composite.
rep(p,
'''    const auto dropletPixel = compiler.Compile({.source=kDropletPixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});\n    if (vertex.bytecode.empty() || pixel.bytecode.empty() || splashVertex.bytecode.empty() || splashPixel.bytecode.empty() || dropletVertex.bytecode.empty() || dropletPixel.bytecode.empty())''',
'''    const auto dropletPixel = compiler.Compile({.source=kDropletPixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});\n    const auto oitCompositeVertex=compiler.Compile({.source=kOitCompositeVertexShader,.entryPoint="main",.stage=shader::Stage::Vertex,.debug=false});\n    const auto oitCompositePixel=compiler.Compile({.source=kOitCompositePixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});\n    if (vertex.bytecode.empty() || pixel.bytecode.empty() || splashVertex.bytecode.empty() || splashPixel.bytecode.empty() || dropletVertex.bytecode.empty() || dropletPixel.bytecode.empty() || oitCompositeVertex.bytecode.empty() || oitCompositePixel.bytecode.empty())''')
# Change three effect pipeline descriptors alpha->additive and 2 MRT formats. Generic exact occurrences.
t=Path(p).read_text(encoding='utf-8')
t=t.replace('.blendMode = rhi::BlendMode::Alpha,\n        .depthCompare', '.blendMode = rhi::BlendMode::Additive,\n        .depthCompare',1)
t=t.replace('''        .colorAttachmentFormats = {\n            rhi::TextureFormat::RGBA16_Float\n        },\n        .colorAttachmentCount = 1U''','''        .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float, rhi::TextureFormat::R16_Float},\n        .colorAttachmentCount = 2U''',1)
t=t.replace('.blendMode=rhi::BlendMode::Alpha,.depthCompare', '.blendMode=rhi::BlendMode::Additive,.depthCompare',2)
t=t.replace('.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},.colorAttachmentCount=1U', '.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float,rhi::TextureFormat::R16_Float},.colorAttachmentCount=2U',2)
Path(p).write_text(t,encoding='utf-8')
# Add composite pipeline after droplet pipeline.
rep(p,
'''    dropletPipeline_=device.CreateGraphicsPipeline({.vertexShader={.data=dropletVertex.bytecode.data(),.size=dropletVertex.bytecode.size()},.pixelShader={.data=dropletPixel.bytecode.data(),.size=dropletPixel.bytecode.size()},.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=20U,.shaderResourceBuffers=8U,.sampledTextures=0U,.topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,.blendMode=rhi::BlendMode::Additive,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=true,.depthWrite=false,.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float,rhi::TextureFormat::R16_Float},.colorAttachmentCount=2U});\n}''',
'''    dropletPipeline_=device.CreateGraphicsPipeline({.vertexShader={.data=dropletVertex.bytecode.data(),.size=dropletVertex.bytecode.size()},.pixelShader={.data=dropletPixel.bytecode.data(),.size=dropletPixel.bytecode.size()},.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=20U,.shaderResourceBuffers=8U,.sampledTextures=0U,.topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,.blendMode=rhi::BlendMode::Additive,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=true,.depthWrite=false,.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float,rhi::TextureFormat::R16_Float},.colorAttachmentCount=2U});\n    oitCompositePipeline_=device.CreateGraphicsPipeline({.vertexShader={.data=oitCompositeVertex.bytecode.data(),.size=oitCompositeVertex.bytecode.size()},.pixelShader={.data=oitCompositePixel.bytecode.data(),.size=oitCompositePixel.bytecode.size()},.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=0U,.shaderResourceBuffers=0U,.sampledTextures=2U,.topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,.blendMode=rhi::BlendMode::Alpha,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=false,.depthWrite=false,.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},.colorAttachmentCount=1U});\n}''')

# Add OIT target cache helper before SetSpawns.
rep(p,
'''void VolumeParticleRenderer::SetSpawns(\n''',
'''VolumeParticleRenderer::OitTargets& VolumeParticleRenderer::EnsureOitTargets(const u32 width,const u32 height,const u32 frameIndex)\n{\n    if(frameIndex>=framesInFlight_) throw std::out_of_range("Orbit M38 OIT frame index exceeds frames in flight.");\n    const u64 key=(static_cast<u64>(width)<<32U)|static_cast<u64>(height);\n    auto& slots=oitTargets_[key];\n    if(slots.empty()) slots.resize(framesInFlight_);\n    auto& target=slots[frameIndex];\n    if(target.accumulation==nullptr){\n        target.accumulation=device_->CreateTexture({.width=width,.height=height,.format=rhi::TextureFormat::RGBA16_Float,.initialState=rhi::ResourceState::ShaderResource});\n        target.opticalDepth=device_->CreateTexture({.width=width,.height=height,.format=rhi::TextureFormat::R16_Float,.initialState=rhi::ResourceState::ShaderResource});\n        target.accumulationState=rhi::ResourceState::ShaderResource; target.opticalDepthState=rhi::ResourceState::ShaderResource;\n    }\n    return target;\n}\n\nvoid VolumeParticleRenderer::SetSpawns(\n''')
# Draw signature.
rep(p,
'''    const render_view::CameraState& camera,\n    const math::Double3 cameraPositionRelativeToPresentationOriginMeters,\n    const f32 radiusPixels)''',
'''    const render_view::CameraState& camera,\n    const math::Double3 cameraPositionRelativeToPresentationOriginMeters,\n    const u32 frameIndex,\n    const f32 radiusPixels)''')
# Insert OIT target setup before SetRenderTargets(sceneColor...).
rep(p,
'''    commands.SetRenderTargets(sceneColor, depth);\n    commands.SetViewport({''',
'''    auto& oit=EnsureOitTargets(width,height,frameIndex);\n    if(oit.accumulationState!=rhi::ResourceState::RenderTarget){commands.Transition(*oit.accumulation,oit.accumulationState,rhi::ResourceState::RenderTarget);oit.accumulationState=rhi::ResourceState::RenderTarget;}\n    if(oit.opticalDepthState!=rhi::ResourceState::RenderTarget){commands.Transition(*oit.opticalDepth,oit.opticalDepthState,rhi::ResourceState::RenderTarget);oit.opticalDepthState=rhi::ResourceState::RenderTarget;}\n    commands.ClearColorTarget(*oit.accumulation,{0,0,0,0});\n    commands.ClearColorTarget(*oit.opticalDepth,{0,0,0,0});\n    std::array<rhi::Texture*,2U> oitColors{oit.accumulation.get(),oit.opticalDepth.get()};\n    commands.SetRenderTargets(oitColors,&depth);\n    commands.SetViewport({''')
# Append resolve after last indirect draw.
rep(p,
'''    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::SplashIndirectOffsetBytes);\n}''',
'''    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::SplashIndirectOffsetBytes);\n\n    commands.Transition(*oit.accumulation,oit.accumulationState,rhi::ResourceState::ShaderResource); oit.accumulationState=rhi::ResourceState::ShaderResource;\n    commands.Transition(*oit.opticalDepth,oit.opticalDepthState,rhi::ResourceState::ShaderResource); oit.opticalDepthState=rhi::ResourceState::ShaderResource;\n    commands.SetRenderTarget(sceneColor);\n    commands.SetGraphicsPipeline(*oitCompositePipeline_);\n    commands.SetGraphicsTexture(0U,*oit.accumulation);\n    commands.SetGraphicsTexture(1U,*oit.opticalDepth);\n    commands.Draw(6U);\n}''')

# Studio call threads real frame-in-flight slot.
p='engine/studio_ui/src/StudioViewportRenderer.cpp'
rep(p,
'''                            height,\n                            camera,\n                            cameraRelativeToParticleOrigin);''',
'''                            height,\n                            camera,\n                            cameraRelativeToParticleOrigin,\n                            particleFrameIndex);''')

# Contract regression.
p='engine/studio_ui/tests/VolumeParticleRenderBridgeTests.cpp'
rep(p,
'''    Check(volume_render::VolumeParticleGpuState::DropletIndirectOffsetBytes == 32U);''',
'''    Check(volume_render::VolumeParticleGpuState::DropletIndirectOffsetBytes == 32U);\n    Check(static_cast<u32>(rhi::BlendMode::Additive) != static_cast<u32>(rhi::BlendMode::Alpha));''')

print('M38 weighted OIT patch applied')
