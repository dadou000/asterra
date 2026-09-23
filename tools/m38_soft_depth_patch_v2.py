from pathlib import Path

def one(path, old, new):
    p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
    if c!=1: raise RuntimeError(f'{path}: expected 1 got {c}: {old[:100]!r}')
    p.write_text(t.replace(old,new,1),encoding='utf-8')

def section_replace(text, begin, end, old, new, expected=1):
    a=text.index(begin); b=text.index(end,a)
    s=text[a:b]; c=s.count(old)
    if c!=expected: raise RuntimeError(f'{begin}: expected {expected} got {c}: {old[:100]!r}')
    s=s.replace(old,new,expected)
    return text[:a]+s+text[b:]

# RHI read-only depth MRT seam.
one('engine/rhi/include/orbit/rhi/Command.hpp',
'''    virtual void SetRenderTargets(\n        std::span<Texture* const> colors,\n        Texture* depth) = 0;\n''',
'''    virtual void SetRenderTargets(\n        std::span<Texture* const> colors,\n        Texture* depth) = 0;\n\n    // Bind MRT color targets with a depth attachment kept in DepthRead so it\n    // can simultaneously be sampled by fragment shaders. Used by soft\n    // particles and other read-only-depth transparent passes.\n    virtual void SetRenderTargetsReadOnlyDepth(\n        std::span<Texture* const> colors,\n        Texture& depth) = 0;\n''')
one('engine/rhi/vulkan/src/VulkanObjects.hpp',
'''    void SetRenderTargets(\n        std::span<Texture* const> colors,\n        Texture* depth) override;\n''',
'''    void SetRenderTargets(\n        std::span<Texture* const> colors,\n        Texture* depth) override;\n\n    void SetRenderTargetsReadOnlyDepth(\n        std::span<Texture* const> colors,\n        Texture& depth) override;\n''')

p=Path('engine/rhi/vulkan/src/VulkanCommands.cpp'); t=p.read_text(encoding='utf-8')
marker='void VulkanCommandList::SetViewport(const Viewport& viewport)\n{'
if t.count(marker)!=1: raise RuntimeError('Vulkan viewport seam')
impl=r'''void VulkanCommandList::SetRenderTargetsReadOnlyDepth(
    const std::span<Texture* const> colors,
    Texture& depth)
{
    if (colors.empty() || colors.size() > 4U)
        throw std::invalid_argument("Orbit Vulkan read-only-depth MRT requires one to four color targets.");

    std::array<VkRenderingAttachmentInfo, 4> nativeColors{};
    u32 width=0U, height=0U;
    for(std::size_t i=0;i<colors.size();++i)
    {
        auto* texture=dynamic_cast<VulkanTexture*>(colors[i]);
        if(texture==nullptr) throw std::runtime_error("Orbit Vulkan received an incompatible MRT color target.");
        if(i==0U){width=texture->Width();height=texture->Height();}
        else if(texture->Width()!=width||texture->Height()!=height) throw std::invalid_argument("Orbit Vulkan MRT color targets must have equal extents.");
        const auto pending=texture->TakePendingClear();
        auto& a=nativeColors[i]; a.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; a.imageView=texture->View(); a.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp=pending.has_value()?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp=VK_ATTACHMENT_STORE_OP_STORE; if(pending.has_value()) a.clearValue=*pending;
    }
    auto* nativeDepth=dynamic_cast<VulkanTexture*>(&depth);
    if(nativeDepth==nullptr) throw std::runtime_error("Orbit Vulkan received an incompatible read-only depth target.");
    if(nativeDepth->Width()!=width||nativeDepth->Height()!=height) throw std::invalid_argument("Orbit Vulkan read-only depth target must match color extents.");
    VkRenderingAttachmentInfo depthAttachment{}; depthAttachment.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; depthAttachment.imageView=nativeDepth->View(); depthAttachment.imageLayout=VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL; depthAttachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; depthAttachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    BeginRendering(std::span<const VkRenderingAttachmentInfo>(nativeColors.data(),colors.size()),&depthAttachment,width,height);
}

'''
t=t.replace(marker,impl+marker,1); p.write_text(t,encoding='utf-8')

# Section-scoped shader edits.
p=Path('engine/volume_render/src/VolumeParticleRenderer.cpp'); t=p.read_text(encoding='utf-8')
PV='constexpr const char* kVertexShader = R"('; SV='constexpr const char* kSplashVertexShader = R"('; SP='constexpr const char* kSplashPixelShader = R"('; DV='constexpr const char* kDropletVertexShader = R"('; DP='constexpr const char* kDropletPixelShader = R"('; PP='constexpr const char* kPixelShader = R"('; OC='constexpr const char* kOitCompositeVertexShader = R"('
# Correct perspective depth in particle/splash/droplet projectors.
t=section_replace(t,PV,SV,
'''    return float4(\n        x / (max(g.projection.x, 0.001) * max(g.projection.y, 0.001)),\n        -y / max(g.projection.y, 0.001),\n        z * 0.5,\n        z);''',
'''    const float n=max(g.projection.z,1.0e-4), f=max(g.projection.w,n+1.0e-3);\n    const float clipZ=z*f/(f-n)-f*n/(f-n);\n    return float4(\n        x / (max(g.projection.x, 0.001) * max(g.projection.y, 0.001)),\n        -y / max(g.projection.y, 0.001),\n        clipZ,\n        z);''')
t=section_replace(t,SV,SP,
'''    return float4(x/(max(g.projection.x,0.001)*max(g.projection.y,0.001)),-y/max(g.projection.y,0.001),z*0.5,z);''',
'''    float n=max(g.projection.z,1.0e-4),f=max(g.projection.w,n+1.0e-3); float clipZ=z*f/(f-n)-f*n/(f-n); return float4(x/(max(g.projection.x,0.001)*max(g.projection.y,0.001)),-y/max(g.projection.y,0.001),clipZ,z);''')
t=section_replace(t,DV,DP,
'''float4 Project(float3 relative){ float3 f=normalize(g.forward.xyz),u=normalize(g.up.xyz),r=normalize(cross(f,u)),cu=normalize(cross(r,f)); float z=dot(relative,f); if(z<=g.projection.z||z>=g.projection.w)return float4(2,2,1,1); return float4(dot(relative,r)/(max(g.projection.x,0.001)*max(g.projection.y,0.001)),-dot(relative,cu)/max(g.projection.y,0.001),z*0.5,z); }''',
'''float4 Project(float3 relative){ float3 fw=normalize(g.forward.xyz),u=normalize(g.up.xyz),r=normalize(cross(fw,u)),cu=normalize(cross(r,fw)); float z=dot(relative,fw); if(z<=g.projection.z||z>=g.projection.w)return float4(2,2,1,1); float n=max(g.projection.z,1.0e-4),f=max(g.projection.w,n+1.0e-3); float clipZ=z*f/(f-n)-f*n/(f-n); return float4(dot(relative,r)/(max(g.projection.x,0.001)*max(g.projection.y,0.001)),-dot(relative,cu)/max(g.projection.y,0.001),clipZ,z); }''')
# Vertex softness outputs.
t=section_replace(t,PV,SV,'    float emissionScale : TEXCOORD7;\n};','    float emissionScale : TEXCOORD7;\n    float softnessMeters : TEXCOORD8;\n};')
t=section_replace(t,PV,SV,'        output.emissionScale = 0.0;\n        return output;','        output.emissionScale = 0.0;\n        output.softnessMeters = 0.0;\n        return output;',2)
t=section_replace(t,PV,SV,'    output.emissionScale = max(particle.emissionScale, 0.0);\n    return output;','    output.emissionScale = max(particle.emissionScale, 0.0);\n    output.softnessMeters = max(particle.radiusMeters * 2.0, 0.05);\n    return output;')
t=section_replace(t,SV,SP,'struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float impact:TEXCOORD2; };','struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float impact:TEXCOORD2; float softnessMeters:TEXCOORD3; };')
t=section_replace(t,SV,SP,'{o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.impact=0;return o;}','{o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.impact=0;o.softnessMeters=0;return o;}')
t=section_replace(t,SV,SP,'{o.position=center;o.uv=0;o.tint=0;o.impact=0;return o;}','{o.position=center;o.uv=0;o.tint=0;o.impact=0;o.softnessMeters=0;return o;}')
t=section_replace(t,SV,SP,'o.uv=corners[cornerIndex]; o.tint=max(e.tint,0.0)*life; o.impact=max(e.impactSpeedMetersPerSecond,0.0)*life; return o;','o.uv=corners[cornerIndex]; o.tint=max(e.tint,0.0)*life; o.impact=max(e.impactSpeedMetersPerSecond,0.0)*life; o.softnessMeters=max(radius*0.25,0.03); return o;')
t=section_replace(t,DV,DP,'struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float life:TEXCOORD2; };','struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float life:TEXCOORD2; float softnessMeters:TEXCOORD3; };')
t=section_replace(t,DV,DP,'{o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.life=0;return o;}','{o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.life=0;o.softnessMeters=0;return o;}')
t=section_replace(t,DV,DP,'{o.position=center;o.uv=0;o.tint=0;o.life=0;return o;}','{o.position=center;o.uv=0;o.tint=0;o.life=0;o.softnessMeters=0;return o;}')
t=section_replace(t,DV,DP,'o.uv=corners[c];o.tint=max(d.tint,0.0);o.life=saturate(1.0-d.ageSeconds/max(d.lifetimeSeconds,0.001));return o;','o.uv=corners[c];o.tint=max(d.tint,0.0);o.life=saturate(1.0-d.ageSeconds/max(d.lifetimeSeconds,0.001));o.softnessMeters=max(d.radiusMeters*4.0,0.02);return o;')
# Pixel structs/bindings/helper.
particle_old='''struct VSOutput\n{\n    float4 position : SV_Position;\n    float2 uv : TEXCOORD0;\n    float authority : TEXCOORD1;\n    float density : TEXCOORD2;\n    float emission : TEXCOORD3;\n    float life : TEXCOORD4;\n    float3 baseColor : TEXCOORD5;\n    float3 emissionColor : TEXCOORD6;\n    float emissionScale : TEXCOORD7;\n};'''
particle_new=particle_old[:-3]+'''    float softnessMeters : TEXCOORD8;\n};\n[[vk::binding(8,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth;\n[[vk::binding(8,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;\nstruct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; }; [[vk::push_constant]] Push g;\nfloat LinearDepth(float d){float n=max(g.projection.z,1e-4),f=max(g.projection.w,n+1e-3);return n*f/max(f-d*(f-n),1e-5);}\nfloat SoftDepth(float4 p,float s){uint w,h;g_sceneDepth.GetDimensions(w,h);int2 q=clamp(int2(p.xy),int2(0,0),int2(max(int(w)-1,0),max(int(h)-1,0)));return saturate((LinearDepth(g_sceneDepth.Load(int3(q,0)).r)-LinearDepth(saturate(p.z)))/max(s,1e-3));}'''
t=section_replace(t,PP,OC,particle_old,particle_new)
t=section_replace(t,PP,OC,'    const float alpha = soft * input.life *\n        saturate(0.16 + 0.64 * authority + 0.20 * density);','    const float depthFade=SoftDepth(input.position,input.softnessMeters);\n    const float alpha = soft * input.life * depthFade *\n        saturate(0.16 + 0.64 * authority + 0.20 * density);')
# splash pixel
t=section_replace(t,SP,DV,'struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float impact:TEXCOORD2; };','struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float impact:TEXCOORD2; float softnessMeters:TEXCOORD3; };\n[[vk::binding(8,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth; [[vk::binding(8,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;\nstruct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; }; [[vk::push_constant]] Push g;\nfloat LinearDepth(float d){float n=max(g.projection.z,1e-4),f=max(g.projection.w,n+1e-3);return n*f/max(f-d*(f-n),1e-5);}\nfloat SoftDepth(float4 p,float s){uint w,h;g_sceneDepth.GetDimensions(w,h);int2 q=clamp(int2(p.xy),int2(0,0),int2(max(int(w)-1,0),max(int(h)-1,0)));return saturate((LinearDepth(g_sceneDepth.Load(int3(q,0)).r)-LinearDepth(saturate(p.z)))/max(s,1e-3));}')
t=section_replace(t,SP,DV,'float impact=saturate(i.impact/8.0); float alpha=ring*(0.35+0.55*impact);','float impact=saturate(i.impact/8.0); float alpha=ring*(0.35+0.55*impact)*SoftDepth(i.position,i.softnessMeters);')
# droplet pixel
t=section_replace(t,DP,PP,'struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float life:TEXCOORD2; };','struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float life:TEXCOORD2; float softnessMeters:TEXCOORD3; };\n[[vk::binding(8,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth; [[vk::binding(8,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;\nstruct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; }; [[vk::push_constant]] Push g;\nfloat LinearDepth(float d){float n=max(g.projection.z,1e-4),f=max(g.projection.w,n+1e-3);return n*f/max(f-d*(f-n),1e-5);}\nfloat SoftDepth(float4 p,float s){uint w,h;g_sceneDepth.GetDimensions(w,h);int2 q=clamp(int2(p.xy),int2(0,0),int2(max(int(w)-1,0),max(int(h)-1,0)));return saturate((LinearDepth(g_sceneDepth.Load(int3(q,0)).r)-LinearDepth(saturate(p.z)))/max(s,1e-3));}')
t=section_replace(t,DP,PP,'float alpha=(1.0-smoothstep(0.2,1.0,r2))*i.life*0.82;','float alpha=(1.0-smoothstep(0.2,1.0,r2))*i.life*0.82*SoftDepth(i.position,i.softnessMeters);')
# Pipeline descriptor bindings + draw setup.
if t.count('.sampledTextures = 0U,')<1: raise RuntimeError('particle sampledTextures seam')
t=t.replace('.sampledTextures = 0U,','.sampledTextures = 1U,',1)
if t.count('.pushConstantDwords=20U,.shaderResourceBuffers=8U,.sampledTextures=0U,')!=2: raise RuntimeError('compact pipeline sampled seam')
t=t.replace('.pushConstantDwords=20U,.shaderResourceBuffers=8U,.sampledTextures=0U,','.pushConstantDwords=20U,.shaderResourceBuffers=8U,.sampledTextures=1U,',2)
if t.count('commands.SetRenderTargets(oitColors,&depth);')!=1: raise RuntimeError('OIT MRT seam')
t=t.replace('commands.SetRenderTargets(oitColors,&depth);','commands.SetRenderTargetsReadOnlyDepth(oitColors,depth);',1)
for off in ['ParticleIndirectOffsetBytes','DropletIndirectOffsetBytes','SplashIndirectOffsetBytes']:
    old=f'''    state_.BindForGraphics(commands);\n    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::{off});'''
    new=f'''    state_.BindForGraphics(commands);\n    commands.SetGraphicsTexture(0U,depth);\n    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::{off});'''
    if t.count(old)!=1: raise RuntimeError(f'draw bind seam {off}')
    t=t.replace(old,new,1)
p.write_text(t,encoding='utf-8')
print('M38 soft-depth v2 applied')
