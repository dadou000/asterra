from pathlib import Path

def replace_once(path, old, new):
    p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
    if c!=1: raise RuntimeError(f'{path}: expected 1 match got {c}: {old[:120]!r}')
    p.write_text(t.replace(old,new,1),encoding='utf-8')

def section_replace(path, begin, end, fn):
    p=Path(path); t=p.read_text(encoding='utf-8'); a=t.index(begin); b=t.index(end,a)
    seg=t[a:b]; seg2=fn(seg)
    p.write_text(t[:a]+seg2+t[b:],encoding='utf-8')

# Header: viewport-keyed temporal history resources.
p='engine/volume_render/include/orbit/volume_render/VolumeParticleRenderer.hpp'
replace_once(p,
'''        math::Double3 cameraPositionRelativeToPresentationOriginMeters,\n        u32 frameIndex,\n        f32 radiusPixels = 3.0F);''',
'''        math::Double3 cameraPositionRelativeToPresentationOriginMeters,\n        u32 frameIndex,\n        u64 temporalHistoryKey,\n        f32 radiusPixels = 3.0F);''')
replace_once(p,
'''        std::unique_ptr<rhi::Texture> opticalDepth;\n        rhi::ResourceState accumulationState{rhi::ResourceState::ShaderResource};\n        rhi::ResourceState opticalDepthState{rhi::ResourceState::ShaderResource};''',
'''        std::unique_ptr<rhi::Texture> opticalDepth;\n        std::unique_ptr<rhi::Texture> motionReject;\n        std::unique_ptr<rhi::Texture> historyA;\n        std::unique_ptr<rhi::Texture> historyB;\n        rhi::ResourceState accumulationState{rhi::ResourceState::ShaderResource};\n        rhi::ResourceState opticalDepthState{rhi::ResourceState::ShaderResource};\n        rhi::ResourceState motionRejectState{rhi::ResourceState::ShaderResource};\n        rhi::ResourceState historyAState{rhi::ResourceState::ShaderResource};\n        rhi::ResourceState historyBState{rhi::ResourceState::ShaderResource};\n        bool writeHistoryA{true};\n        bool hasHistory{false};\n        math::Double3 previousCameraPositionMeters{};\n        math::Float3 previousForward{0.0F, 0.0F, 1.0F};\n        u32 temporalSequence{0U};''')
replace_once(p,
'''    [[nodiscard]] OitTargets& EnsureOitTargets(u32 width, u32 height, u32 frameIndex);''',
'''    [[nodiscard]] OitTargets& EnsureOitTargets(u32 width, u32 height, u32 frameIndex, u64 temporalHistoryKey);''')
replace_once(p,
'''    std::unique_ptr<rhi::GraphicsPipeline> oitCompositePipeline_;''',
'''    std::unique_ptr<rhi::GraphicsPipeline> oitTemporalPipeline_;\n    std::unique_ptr<rhi::GraphicsPipeline> oitCompositePipeline_;''')
replace_once(p,
'''    std::map<u64, std::vector<OitTargets>> oitTargets_;''',
'''    std::map<std::pair<u64, u64>, std::vector<OitTargets>> oitTargets_;''')

# Renderer source: shader updates and temporal resolve.
p='engine/volume_render/src/VolumeParticleRenderer.cpp'

# Extend push structs from 5 float4 to 6 in effect shaders.
def add_temporal_push(seg):
    seg=seg.replace('    float4 viewport;\n};','    float4 viewport;\n    float4 temporal;\n};',1)
    return seg
section_replace(p,'constexpr const char* kVertexShader = R"(',')";\n\n\nconstexpr const char* kSplashVertexShader',add_temporal_push)
for begin,end in [
 ('constexpr const char* kSplashVertexShader = R"(',')";\nconstexpr const char* kSplashPixelShader'),
 ('constexpr const char* kSplashPixelShader = R"(',')";\n\nconstexpr const char* kDropletVertexShader'),
 ('constexpr const char* kDropletVertexShader = R"(',')";\nconstexpr const char* kDropletPixelShader'),
 ('constexpr const char* kDropletPixelShader = R"(',')";\n\nconstexpr const char* kPixelShader'),
 ('constexpr const char* kPixelShader = R"(',')";\n\nconstexpr const char* kOitCompositeVertexShader')]:
    def f(seg):
        return seg.replace('struct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; };','struct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; float4 temporal; };',1)
    section_replace(p,begin,end,f)

# Particle vertex: coverage output and value.
def particle_vs(seg):
    seg=seg.replace('    float softnessMeters : TEXCOORD8;\n};','    float softnessMeters : TEXCOORD8;\n    float stochasticCoverage : TEXCOORD9;\n};',1)
    seg=seg.replace('        output.softnessMeters = 0.0;\n        return output;','        output.softnessMeters = 0.0;\n        output.stochasticCoverage = 0.0;\n        return output;',2)
    seg=seg.replace('    output.softnessMeters = max(particle.radiusMeters * 2.0, 0.05);\n    return output;','    output.softnessMeters = max(particle.radiusMeters * 2.0, 0.05);\n    output.stochasticCoverage = saturate((projectedRadiusPixels * projectedRadiusPixels) / max(radiusPixels * radiusPixels, 1.0e-4));\n    return output;',1)
    return seg
section_replace(p,'constexpr const char* kVertexShader = R"(',')";\n\n\nconstexpr const char* kSplashVertexShader',particle_vs)

# Droplet vertex coverage.
def droplet_vs(seg):
    seg=seg.replace('float softnessMeters:TEXCOORD3; };','float softnessMeters:TEXCOORD3; float stochasticCoverage:TEXCOORD4; };',1)
    seg=seg.replace('o.softnessMeters=0;return o;','o.softnessMeters=0;o.stochasticCoverage=0;return o;',2)
    old='float px=max(d.radiusMeters,0.002)/max(center.w*max(g.projection.y,0.001),0.001)*max(g.viewport.y,1.0)*0.5; px=max(px,0.75);'
    new='float projectedPx=max(d.radiusMeters,0.002)/max(center.w*max(g.projection.y,0.001),0.001)*max(g.viewport.y,1.0)*0.5; float px=max(projectedPx,0.75);'
    seg=seg.replace(old,new,1)
    seg=seg.replace('o.softnessMeters=max(d.radiusMeters*4.0,0.02);return o;','o.softnessMeters=max(d.radiusMeters*4.0,0.02);o.stochasticCoverage=saturate((projectedPx*projectedPx)/max(px*px,1e-4));return o;',1)
    return seg
section_replace(p,'constexpr const char* kDropletVertexShader = R"(',')";\nconstexpr const char* kDropletPixelShader',droplet_vs)

# All OIT pixel outputs gain motion-reject target. Particles/splash write zero, droplets write alpha.
def oit_particle_ps(seg):
    seg=seg.replace('    float softnessMeters : TEXCOORD8;\n};','    float softnessMeters : TEXCOORD8;\n    float stochasticCoverage : TEXCOORD9;\n};',1)
    seg=seg.replace('struct OitOutput { float4 accumulation : SV_Target0; float4 opticalDepth : SV_Target1; };','struct OitOutput { float4 accumulation : SV_Target0; float4 opticalDepth : SV_Target1; float4 motionReject : SV_Target2; };',1)
    seg=seg.replace('OitOutput main(VSOutput input)\n{','float Hash12(float2 p,uint seed){uint x=asuint(p.x)*1664525u+asuint(p.y)*1013904223u+seed*747796405u;x^=x>>16;x*=2246822519u;x^=x>>13;return float(x&0x00ffffffu)/16777216.0;}\nOitOutput main(VSOutput input)\n{',1)
    seg=seg.replace('    const float soft = 1.0 - smoothstep(0.30, 1.0, radius2);','    if(input.stochasticCoverage<0.999 && Hash12(floor(input.position.xy),asuint(g.temporal.x))>input.stochasticCoverage) discard;\n    const float soft = 1.0 - smoothstep(0.30, 1.0, radius2);',1)
    seg=seg.replace('    output.opticalDepth = float4(opticalDepth, 0.0, 0.0, 0.0);\n    return output;','    output.opticalDepth = float4(opticalDepth, 0.0, 0.0, 0.0);\n    output.motionReject = 0.0;\n    return output;',1)
    return seg
section_replace(p,'constexpr const char* kPixelShader = R"(',')";\n\nconstexpr const char* kOitCompositeVertexShader',oit_particle_ps)

def splash_ps(seg):
    seg=seg.replace('struct OitOutput { float4 accumulation:SV_Target0; float4 opticalDepth:SV_Target1; };','struct OitOutput { float4 accumulation:SV_Target0; float4 opticalDepth:SV_Target1; float4 motionReject:SV_Target2; };',1)
    seg=seg.replace('o.accumulation=float4(foam*alpha,alpha); o.opticalDepth=float4(optical,0,0,0); return o;','o.accumulation=float4(foam*alpha,alpha); o.opticalDepth=float4(optical,0,0,0); o.motionReject=0; return o;',1)
    return seg
section_replace(p,'constexpr const char* kSplashPixelShader = R"(',')";\n\nconstexpr const char* kDropletVertexShader',splash_ps)

def droplet_ps(seg):
    seg=seg.replace('float softnessMeters:TEXCOORD3; };','float softnessMeters:TEXCOORD3; float stochasticCoverage:TEXCOORD4; };',1)
    seg=seg.replace('struct OitOutput { float4 accumulation:SV_Target0; float4 opticalDepth:SV_Target1; };','struct OitOutput { float4 accumulation:SV_Target0; float4 opticalDepth:SV_Target1; float4 motionReject:SV_Target2; };',1)
    seg=seg.replace('OitOutput main(VSOutput i) {','float Hash12(float2 p,uint seed){uint x=asuint(p.x)*1664525u+asuint(p.y)*1013904223u+seed*747796405u;x^=x>>16;x*=2246822519u;x^=x>>13;return float(x&0x00ffffffu)/16777216.0;}\nOitOutput main(VSOutput i) {',1)
    seg=seg.replace('float r2=dot(i.uv,i.uv); if(r2>=1.0||i.life<=0.0) discard;','float r2=dot(i.uv,i.uv); if(r2>=1.0||i.life<=0.0) discard; if(i.stochasticCoverage<0.999 && Hash12(floor(i.position.xy),asuint(g.temporal.x))>i.stochasticCoverage) discard;',1)
    seg=seg.replace('o.accumulation=float4(c*alpha,alpha); o.opticalDepth=float4(optical,0,0,0); return o;','o.accumulation=float4(c*alpha,alpha); o.opticalDepth=float4(optical,0,0,0); o.motionReject=float4(alpha,0,0,0); return o;',1)
    return seg
section_replace(p,'constexpr const char* kDropletPixelShader = R"(',')";\n\nconstexpr const char* kPixelShader',droplet_ps)

# Replace old OIT composite shader with temporal resolve + simple present.
replace_once(p,
'''constexpr const char* kOitCompositePixelShader = R"(\nstruct O { float4 position:SV_Position; float2 uv:TEXCOORD0; };\n[[vk::binding(0,0)]][[vk::combinedImageSampler]] Texture2D g_accum;\n[[vk::binding(0,0)]][[vk::combinedImageSampler]] SamplerState g_accumSampler;\n[[vk::binding(1,0)]][[vk::combinedImageSampler]] Texture2D g_optical;\n[[vk::binding(1,0)]][[vk::combinedImageSampler]] SamplerState g_opticalSampler;\nfloat4 main(O i):SV_Target0 { float4 a=g_accum.Sample(g_accumSampler,i.uv); float optical=max(g_optical.Sample(g_opticalSampler,i.uv).r,0.0); if(a.a<=1.0e-6||optical<=1.0e-6) discard; float3 color=a.rgb/max(a.a,1.0e-6); float alpha=1.0-exp(-min(optical,20.0)); return float4(color,alpha); }\n)";''',
'''constexpr const char* kOitTemporalPixelShader = R"(\nstruct O { float4 position:SV_Position; float2 uv:TEXCOORD0; };\n[[vk::binding(0,0)]][[vk::combinedImageSampler]] Texture2D g_accum; [[vk::binding(0,0)]][[vk::combinedImageSampler]] SamplerState s0;\n[[vk::binding(1,0)]][[vk::combinedImageSampler]] Texture2D g_optical; [[vk::binding(1,0)]][[vk::combinedImageSampler]] SamplerState s1;\n[[vk::binding(2,0)]][[vk::combinedImageSampler]] Texture2D g_reject; [[vk::binding(2,0)]][[vk::combinedImageSampler]] SamplerState s2;\n[[vk::binding(3,0)]][[vk::combinedImageSampler]] Texture2D g_history; [[vk::binding(3,0)]][[vk::combinedImageSampler]] SamplerState s3;\nstruct TemporalPush { float4 params; }; [[vk::push_constant]] TemporalPush g;\nfloat4 main(O i):SV_Target0 { float4 a=g_accum.Sample(s0,i.uv); float optical=max(g_optical.Sample(s1,i.uv).r,0.0); float3 color=a.a>1e-6?a.rgb/max(a.a,1e-6):0; float alpha=1.0-exp(-min(optical,20.0)); float4 current=float4(color,alpha); float reject=saturate(g_reject.Sample(s2,i.uv).r*2.0); float4 history=g_history.Sample(s3,i.uv); float localChange=saturate(abs(current.a-history.a)*3.0 + length(current.rgb-history.rgb)*0.25); float historyWeight=saturate(g.params.x)*(1.0-reject)*(1.0-localChange); return lerp(current,history,historyWeight); }\n)";\nconstexpr const char* kOitCompositePixelShader = R"(\nstruct O { float4 position:SV_Position; float2 uv:TEXCOORD0; };\n[[vk::binding(0,0)]][[vk::combinedImageSampler]] Texture2D g_history; [[vk::binding(0,0)]][[vk::combinedImageSampler]] SamplerState s0;\nfloat4 main(O i):SV_Target0 { float4 c=g_history.Sample(s0,i.uv); if(c.a<=1.0e-6) discard; return c; }\n)";''')

# Compile temporal shader and pipeline.
replace_once(p,
'''    const auto oitCompositePixel=compiler.Compile({.source=kOitCompositePixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});''',
'''    const auto oitTemporalPixel=compiler.Compile({.source=kOitTemporalPixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});\n    const auto oitCompositePixel=compiler.Compile({.source=kOitCompositePixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});''')
replace_once(p,
'''dropletPixel.bytecode.empty() || oitCompositeVertex.bytecode.empty() || oitCompositePixel.bytecode.empty())''',
'''dropletPixel.bytecode.empty() || oitCompositeVertex.bytecode.empty() || oitTemporalPixel.bytecode.empty() || oitCompositePixel.bytecode.empty())''')
# Pipeline formats/count and push size.
t=Path(p).read_text(encoding='utf-8')
t=t.replace('.pushConstantDwords = 20U,','.pushConstantDwords = 24U,',1)
t=t.replace('.colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float, rhi::TextureFormat::R16_Float},\n        .colorAttachmentCount = 2U','.colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float, rhi::TextureFormat::R16_Float, rhi::TextureFormat::R16_Float},\n        .colorAttachmentCount = 3U',1)
t=t.replace('.pushConstantDwords=20U,.shaderResourceBuffers=8U,.sampledTextures=1U,','.pushConstantDwords=24U,.shaderResourceBuffers=8U,.sampledTextures=1U,',2)
t=t.replace('.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float,rhi::TextureFormat::R16_Float},.colorAttachmentCount=2U','.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float,rhi::TextureFormat::R16_Float,rhi::TextureFormat::R16_Float},.colorAttachmentCount=3U',2)
old='''    oitCompositePipeline_=device.CreateGraphicsPipeline({.vertexShader={.data=oitCompositeVertex.bytecode.data(),.size=oitCompositeVertex.bytecode.size()},.pixelShader={.data=oitCompositePixel.bytecode.data(),.size=oitCompositePixel.bytecode.size()},.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=0U,.shaderResourceBuffers=0U,.sampledTextures=2U,.topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,.blendMode=rhi::BlendMode::Alpha,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=false,.depthWrite=false,.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},.colorAttachmentCount=1U});'''
new='''    oitTemporalPipeline_=device.CreateGraphicsPipeline({.vertexShader={.data=oitCompositeVertex.bytecode.data(),.size=oitCompositeVertex.bytecode.size()},.pixelShader={.data=oitTemporalPixel.bytecode.data(),.size=oitTemporalPixel.bytecode.size()},.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=4U,.shaderResourceBuffers=0U,.sampledTextures=4U,.topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,.blendMode=rhi::BlendMode::Opaque,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=false,.depthWrite=false,.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},.colorAttachmentCount=1U});\n    oitCompositePipeline_=device.CreateGraphicsPipeline({.vertexShader={.data=oitCompositeVertex.bytecode.data(),.size=oitCompositeVertex.bytecode.size()},.pixelShader={.data=oitCompositePixel.bytecode.data(),.size=oitCompositePixel.bytecode.size()},.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=0U,.shaderResourceBuffers=0U,.sampledTextures=1U,.topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,.blendMode=rhi::BlendMode::Alpha,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=false,.depthWrite=false,.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},.colorAttachmentCount=1U});'''
if old not in t: raise RuntimeError('pipeline seam missing')
t=t.replace(old,new,1)
Path(p).write_text(t,encoding='utf-8')

# EnsureOitTargets signature, key, and resources.
replace_once(p,'VolumeParticleRenderer::OitTargets& VolumeParticleRenderer::EnsureOitTargets(const u32 width,const u32 height,const u32 frameIndex)','VolumeParticleRenderer::OitTargets& VolumeParticleRenderer::EnsureOitTargets(const u32 width,const u32 height,const u32 frameIndex,const u64 temporalHistoryKey)')
replace_once(p,
'''    const u64 key=(static_cast<u64>(width)<<32U)|static_cast<u64>(height);\n    auto& slots=oitTargets_[key];''',
'''    const u64 dimensions=(static_cast<u64>(width)<<32U)|static_cast<u64>(height);\n    auto& slots=oitTargets_[{temporalHistoryKey,dimensions}];''')
replace_once(p,
'''        target.opticalDepth=device_->CreateTexture({.width=width,.height=height,.format=rhi::TextureFormat::R16_Float,.initialState=rhi::ResourceState::ShaderResource});\n        target.accumulationState=rhi::ResourceState::ShaderResource; target.opticalDepthState=rhi::ResourceState::ShaderResource;''',
'''        target.opticalDepth=device_->CreateTexture({.width=width,.height=height,.format=rhi::TextureFormat::R16_Float,.initialState=rhi::ResourceState::ShaderResource});\n        target.motionReject=device_->CreateTexture({.width=width,.height=height,.format=rhi::TextureFormat::R16_Float,.initialState=rhi::ResourceState::ShaderResource});\n        target.historyA=device_->CreateTexture({.width=width,.height=height,.format=rhi::TextureFormat::RGBA16_Float,.initialState=rhi::ResourceState::ShaderResource});\n        target.historyB=device_->CreateTexture({.width=width,.height=height,.format=rhi::TextureFormat::RGBA16_Float,.initialState=rhi::ResourceState::ShaderResource});\n        target.accumulationState=rhi::ResourceState::ShaderResource; target.opticalDepthState=rhi::ResourceState::ShaderResource; target.motionRejectState=rhi::ResourceState::ShaderResource; target.historyAState=rhi::ResourceState::ShaderResource; target.historyBState=rhi::ResourceState::ShaderResource;''')

# Draw signature and constants.
replace_once(p,
'''    const math::Double3 cameraPositionRelativeToPresentationOriginMeters,\n    const u32 frameIndex,\n    const f32 radiusPixels)''',
'''    const math::Double3 cameraPositionRelativeToPresentationOriginMeters,\n    const u32 frameIndex,\n    const u64 temporalHistoryKey,\n    const f32 radiusPixels)''')
replace_once(p,'    std::array<u32, 20> constants{};','    std::array<u32, 24> constants{};')
replace_once(p,'    auto& oit=EnsureOitTargets(width,height,frameIndex);','    auto& oit=EnsureOitTargets(width,height,frameIndex,temporalHistoryKey);\n    ++oit.temporalSequence; if(oit.temporalSequence==0U) oit.temporalSequence=1U;\n    constants[20]=oit.temporalSequence;')

# Transition/clear third OIT target and 3 MRTs.
replace_once(p,
'''    if(oit.opticalDepthState!=rhi::ResourceState::RenderTarget){commands.Transition(*oit.opticalDepth,oit.opticalDepthState,rhi::ResourceState::RenderTarget);oit.opticalDepthState=rhi::ResourceState::RenderTarget;}\n    commands.ClearColorTarget(*oit.accumulation,{0,0,0,0});\n    commands.ClearColorTarget(*oit.opticalDepth,{0,0,0,0});\n    std::array<rhi::Texture*,2U> oitColors{oit.accumulation.get(),oit.opticalDepth.get()};''',
'''    if(oit.opticalDepthState!=rhi::ResourceState::RenderTarget){commands.Transition(*oit.opticalDepth,oit.opticalDepthState,rhi::ResourceState::RenderTarget);oit.opticalDepthState=rhi::ResourceState::RenderTarget;}\n    if(oit.motionRejectState!=rhi::ResourceState::RenderTarget){commands.Transition(*oit.motionReject,oit.motionRejectState,rhi::ResourceState::RenderTarget);oit.motionRejectState=rhi::ResourceState::RenderTarget;}\n    commands.ClearColorTarget(*oit.accumulation,{0,0,0,0});\n    commands.ClearColorTarget(*oit.opticalDepth,{0,0,0,0});\n    commands.ClearColorTarget(*oit.motionReject,{0,0,0,0});\n    std::array<rhi::Texture*,3U> oitColors{oit.accumulation.get(),oit.opticalDepth.get(),oit.motionReject.get()};''')

# Replace final direct resolve with temporal history + present.
old='''    commands.Transition(*oit.accumulation,oit.accumulationState,rhi::ResourceState::ShaderResource); oit.accumulationState=rhi::ResourceState::ShaderResource;\n    commands.Transition(*oit.opticalDepth,oit.opticalDepthState,rhi::ResourceState::ShaderResource); oit.opticalDepthState=rhi::ResourceState::ShaderResource;\n    commands.SetRenderTarget(sceneColor);\n    commands.SetGraphicsPipeline(*oitCompositePipeline_);\n    commands.SetGraphicsTexture(0U,*oit.accumulation);\n    commands.SetGraphicsTexture(1U,*oit.opticalDepth);\n    commands.Draw(6U);'''
new='''    commands.Transition(*oit.accumulation,oit.accumulationState,rhi::ResourceState::ShaderResource); oit.accumulationState=rhi::ResourceState::ShaderResource;\n    commands.Transition(*oit.opticalDepth,oit.opticalDepthState,rhi::ResourceState::ShaderResource); oit.opticalDepthState=rhi::ResourceState::ShaderResource;\n    commands.Transition(*oit.motionReject,oit.motionRejectState,rhi::ResourceState::ShaderResource); oit.motionRejectState=rhi::ResourceState::ShaderResource;\n\n    auto* historyWrite=oit.writeHistoryA?oit.historyA.get():oit.historyB.get();\n    auto* historyRead=oit.writeHistoryA?oit.historyB.get():oit.historyA.get();\n    auto& historyWriteState=oit.writeHistoryA?oit.historyAState:oit.historyBState;\n    auto& historyReadState=oit.writeHistoryA?oit.historyBState:oit.historyAState;\n    if(!oit.hasHistory){\n        if(historyReadState!=rhi::ResourceState::RenderTarget){commands.Transition(*historyRead,historyReadState,rhi::ResourceState::RenderTarget);historyReadState=rhi::ResourceState::RenderTarget;}\n        commands.ClearColorTarget(*historyRead,{0,0,0,0});\n        commands.SetRenderTarget(*historyRead);\n        if(historyReadState!=rhi::ResourceState::ShaderResource){commands.Transition(*historyRead,historyReadState,rhi::ResourceState::ShaderResource);historyReadState=rhi::ResourceState::ShaderResource;}\n    }\n    if(historyWriteState!=rhi::ResourceState::RenderTarget){commands.Transition(*historyWrite,historyWriteState,rhi::ResourceState::RenderTarget);historyWriteState=rhi::ResourceState::RenderTarget;}\n\n    float historyWeight=0.0F;\n    if(oit.hasHistory){\n        const f64 dx=camera.localPositionMeters.x-oit.previousCameraPositionMeters.x,dy=camera.localPositionMeters.y-oit.previousCameraPositionMeters.y,dz=camera.localPositionMeters.z-oit.previousCameraPositionMeters.z;\n        const f64 translation=std::sqrt(dx*dx+dy*dy+dz*dz);\n        const f32 dot=std::clamp(camera.forward.x*oit.previousForward.x+camera.forward.y*oit.previousForward.y+camera.forward.z*oit.previousForward.z,-1.0F,1.0F);\n        const f32 angle=std::acos(dot);\n        historyWeight=0.90F*std::exp(-static_cast<f32>(translation)*3.0F)*std::exp(-angle*48.0F);\n    }\n    std::array<u32,4U> temporalConstants{}; temporalConstants[0]=bits(historyWeight);\n    commands.SetRenderTarget(*historyWrite);\n    commands.SetGraphicsPipeline(*oitTemporalPipeline_);\n    commands.SetGraphicsConstants(temporalConstants);\n    commands.SetGraphicsTexture(0U,*oit.accumulation); commands.SetGraphicsTexture(1U,*oit.opticalDepth); commands.SetGraphicsTexture(2U,*oit.motionReject); commands.SetGraphicsTexture(3U,*historyRead);\n    commands.Draw(6U);\n    commands.Transition(*historyWrite,historyWriteState,rhi::ResourceState::ShaderResource); historyWriteState=rhi::ResourceState::ShaderResource;\n\n    commands.SetRenderTarget(sceneColor);\n    commands.SetGraphicsPipeline(*oitCompositePipeline_);\n    commands.SetGraphicsTexture(0U,*historyWrite);\n    commands.Draw(6U);\n    oit.previousCameraPositionMeters=camera.localPositionMeters; oit.previousForward=camera.forward; oit.hasHistory=true; oit.writeHistoryA=!oit.writeHistoryA;'''
if old not in Path(p).read_text(encoding='utf-8'): raise RuntimeError('final resolve seam missing')
replace_once(p,old,new)

# Reset temporal history too.
replace_once(p,
'''    state_.Reset();\n    terrainCollisionPages_.clear();''',
'''    state_.Reset();\n    terrainCollisionPages_.clear();\n    oitTargets_.clear();''')

# Studio passes stable viewport key.
p='engine/studio_ui/src/StudioViewportRenderer.cpp'
# capture a precomputed key next to particleFrameIndex.
replace_once(p,
'''            const u32 particleFrameIndex =\n                frameIndex % framesInFlight_;''',
'''            const u32 particleFrameIndex =\n                frameIndex % framesInFlight_;\n            const u64 particleTemporalHistoryKey =\n                StableViewportHash(info.id);''')
replace_once(p,
'''                     camera,\n                     cameraRelativeToParticleOrigin,\n                     particleFrameIndex,''',
'''                     camera,\n                     cameraRelativeToParticleOrigin,\n                     particleFrameIndex,\n                     particleTemporalHistoryKey,''')
replace_once(p,
'''                            camera,\n                            cameraRelativeToParticleOrigin);''',
'''                            camera,\n                            cameraRelativeToParticleOrigin,\n                            particleFrameIndex,\n                            particleTemporalHistoryKey);''')

print('M38 temporal OIT patch applied')
