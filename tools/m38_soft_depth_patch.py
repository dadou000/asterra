from pathlib import Path

def rep(path,old,new):
 p=Path(path); t=p.read_text(encoding='utf-8'); c=t.count(old)
 if c!=1: raise RuntimeError(f'{path}: expected 1 match got {c}: {old[:120]!r}')
 p.write_text(t.replace(old,new,1),encoding='utf-8')

# RHI: explicit read-only depth MRT binding so depth can also be sampled.
p='engine/rhi/include/orbit/rhi/Command.hpp'
rep(p,
'''    virtual void SetRenderTargets(\n        std::span<Texture* const> colors,\n        Texture* depth) = 0;\n''',
'''    virtual void SetRenderTargets(\n        std::span<Texture* const> colors,\n        Texture* depth) = 0;\n\n    // MRT binding with a depth attachment that remains in DepthRead and may\n    // simultaneously be sampled by fragment shaders. Pipelines used with this\n    // entry point must have depthWrite=false.\n    virtual void SetRenderTargetsReadOnlyDepth(\n        std::span<Texture* const> colors,\n        Texture& depth) = 0;\n''')

p='engine/rhi/vulkan/src/VulkanObjects.hpp'
rep(p,
'''    void SetRenderTargets(\n        std::span<Texture* const> colors,\n        Texture* depth) override;\n''',
'''    void SetRenderTargets(\n        std::span<Texture* const> colors,\n        Texture* depth) override;\n\n    void SetRenderTargetsReadOnlyDepth(\n        std::span<Texture* const> colors,\n        Texture& depth) override;\n''')

# Vulkan implementation: duplicate MRT setup but use DEPTH_READ_ONLY layout.
p='engine/rhi/vulkan/src/VulkanCommands.cpp'
marker='''void VulkanCommandList::SetViewport(const Viewport& viewport)\n{'''
impl=r'''void VulkanCommandList::SetRenderTargetsReadOnlyDepth(
    const std::span<Texture* const> colors,
    Texture& depth)
{
    if (colors.empty() || colors.size() > 4U)
    {
        throw std::invalid_argument(
            "Orbit Vulkan read-only-depth MRT requires between one and four color targets.");
    }

    std::array<VkRenderingAttachmentInfo, 4> nativeColors{};
    u32 width = 0U;
    u32 height = 0U;
    for (std::size_t index = 0; index < colors.size(); ++index)
    {
        auto* texture = dynamic_cast<VulkanTexture*>(colors[index]);
        if (texture == nullptr)
            throw std::runtime_error("Orbit Vulkan received an incompatible MRT color target.");
        if (index == 0U) { width = texture->Width(); height = texture->Height(); }
        else if (texture->Width() != width || texture->Height() != height)
            throw std::invalid_argument("Orbit Vulkan MRT color targets must have equal extents.");
        const auto pendingClear = texture->TakePendingClear();
        auto& attachment = nativeColors[index];
        attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment.imageView = texture->View();
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = pendingClear.has_value() ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (pendingClear.has_value()) attachment.clearValue = *pendingClear;
    }

    auto* nativeDepth = dynamic_cast<VulkanTexture*>(&depth);
    if (nativeDepth == nullptr)
        throw std::runtime_error("Orbit Vulkan received an incompatible read-only depth target.");
    if (nativeDepth->Width() != width || nativeDepth->Height() != height)
        throw std::invalid_argument("Orbit Vulkan read-only depth target must match color extents.");

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = nativeDepth->View();
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    BeginRendering(
        std::span<const VkRenderingAttachmentInfo>(nativeColors.data(), colors.size()),
        &depthAttachment,
        width,
        height);
}

'''
rep(p,marker,impl+marker)

# Particle renderer: proper perspective depth, sampled depth, physical-ish softness.
p='engine/volume_render/src/VolumeParticleRenderer.cpp'
# Fix all three projection functions from constant 0.5 depth to standard [0,1] perspective depth.
old='''    return float4(\n        x / (max(g.projection.x, 0.001) * max(g.projection.y, 0.001)),\n        -y / max(g.projection.y, 0.001),\n        z * 0.5,\n        z);'''
new='''    const float nearZ=max(g.projection.z,1.0e-4);\n    const float farZ=max(g.projection.w,nearZ+1.0e-3);\n    const float clipZ=z*farZ/(farZ-nearZ)-farZ*nearZ/(farZ-nearZ);\n    return float4(\n        x / (max(g.projection.x, 0.001) * max(g.projection.y, 0.001)),\n        -y / max(g.projection.y, 0.001),\n        clipZ,\n        z);'''
rep(p,old,new)
# Compact Project functions for splash and droplets.
rep(p,
'''    return float4(x/(max(g.projection.x,0.001)*max(g.projection.y,0.001)),-y/max(g.projection.y,0.001),z*0.5,z);''',
'''    float n=max(g.projection.z,1.0e-4), f=max(g.projection.w,n+1.0e-3); float clipZ=z*f/(f-n)-f*n/(f-n);\n    return float4(x/(max(g.projection.x,0.001)*max(g.projection.y,0.001)),-y/max(g.projection.y,0.001),clipZ,z);''')
rep(p,
'''float4 Project(float3 relative){ float3 f=normalize(g.forward.xyz),u=normalize(g.up.xyz),r=normalize(cross(f,u)),cu=normalize(cross(r,f)); float z=dot(relative,f); if(z<=g.projection.z||z>=g.projection.w)return float4(2,2,1,1); return float4(dot(relative,r)/(max(g.projection.x,0.001)*max(g.projection.y,0.001)),-dot(relative,cu)/max(g.projection.y,0.001),z*0.5,z); }''',
'''float4 Project(float3 relative){ float3 fw=normalize(g.forward.xyz),u=normalize(g.up.xyz),r=normalize(cross(fw,u)),cu=normalize(cross(r,fw)); float z=dot(relative,fw); if(z<=g.projection.z||z>=g.projection.w)return float4(2,2,1,1); float n=max(g.projection.z,1.0e-4),f=max(g.projection.w,n+1.0e-3); float clipZ=z*f/(f-n)-f*n/(f-n); return float4(dot(relative,r)/(max(g.projection.x,0.001)*max(g.projection.y,0.001)),-dot(relative,cu)/max(g.projection.y,0.001),clipZ,z); }''')

# Particle VS softness interpolant.
rep(p,'    float emissionScale : TEXCOORD7;\n};','    float emissionScale : TEXCOORD7;\n    float softnessMeters : TEXCOORD8;\n};',)
# initialize invalid paths (three occurrences in particle VS)
rep(p,'        output.emissionScale = 0.0;\n        return output;','        output.emissionScale = 0.0;\n        output.softnessMeters = 0.0;\n        return output;')
rep(p,'        output.emissionScale = 0.0;\n        return output;','        output.emissionScale = 0.0;\n        output.softnessMeters = 0.0;\n        return output;')
rep(p,'    output.emissionScale = max(particle.emissionScale, 0.0);\n    return output;','    output.emissionScale = max(particle.emissionScale, 0.0);\n    output.softnessMeters = max(particle.radiusMeters * 2.0, 0.05);\n    return output;')

# Splash/droplet VS structs + values.
rep(p,'struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float impact:TEXCOORD2; };','struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float impact:TEXCOORD2; float softnessMeters:TEXCOORD3; };')
rep(p,'{o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.impact=0;return o;}','{o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.impact=0;o.softnessMeters=0;return o;}')
rep(p,'{o.position=center;o.uv=0;o.tint=0;o.impact=0;return o;}','{o.position=center;o.uv=0;o.tint=0;o.impact=0;o.softnessMeters=0;return o;}')
rep(p,'o.uv=corners[cornerIndex]; o.tint=max(e.tint,0.0)*life; o.impact=max(e.impactSpeedMetersPerSecond,0.0)*life; return o;','o.uv=corners[cornerIndex]; o.tint=max(e.tint,0.0)*life; o.impact=max(e.impactSpeedMetersPerSecond,0.0)*life; o.softnessMeters=max(radius*0.25,0.03); return o;')
rep(p,'struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float life:TEXCOORD2; };','struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float life:TEXCOORD2; float softnessMeters:TEXCOORD3; };')
rep(p,'{o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.life=0;return o;}','{o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.life=0;o.softnessMeters=0;return o;}')
rep(p,'{o.position=center;o.uv=0;o.tint=0;o.life=0;return o;}','{o.position=center;o.uv=0;o.tint=0;o.life=0;o.softnessMeters=0;return o;}')
rep(p,'o.uv=corners[c];o.tint=max(d.tint,0.0);o.life=saturate(1.0-d.ageSeconds/max(d.lifetimeSeconds,0.001));return o;','o.uv=corners[c];o.tint=max(d.tint,0.0);o.life=saturate(1.0-d.ageSeconds/max(d.lifetimeSeconds,0.001));o.softnessMeters=max(d.radiusMeters*4.0,0.02);return o;')

# Add shared depth binding/helpers separately to each pixel shader source via struct strings.
particle_struct='''struct VSOutput\n{\n    float4 position : SV_Position;\n    float2 uv : TEXCOORD0;\n    float authority : TEXCOORD1;\n    float density : TEXCOORD2;\n    float emission : TEXCOORD3;\n    float life : TEXCOORD4;\n    float3 baseColor : TEXCOORD5;\n    float3 emissionColor : TEXCOORD6;\n    float emissionScale : TEXCOORD7;\n};'''
particle_struct_new='''struct VSOutput\n{\n    float4 position : SV_Position;\n    float2 uv : TEXCOORD0;\n    float authority : TEXCOORD1;\n    float density : TEXCOORD2;\n    float emission : TEXCOORD3;\n    float life : TEXCOORD4;\n    float3 baseColor : TEXCOORD5;\n    float3 emissionColor : TEXCOORD6;\n    float emissionScale : TEXCOORD7;\n    float softnessMeters : TEXCOORD8;\n};\n[[vk::binding(8,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth;\n[[vk::binding(8,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;\nstruct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; }; [[vk::push_constant]] Push g;\nfloat LinearDepth(float d){ float n=max(g.projection.z,1.0e-4),f=max(g.projection.w,n+1.0e-3); return n*f/max(f-d*(f-n),1.0e-5); }\nfloat SoftDepth(float4 p,float softness){ uint w,h; g_sceneDepth.GetDimensions(w,h); int2 q=clamp(int2(p.xy),int2(0,0),int2(max(int(w)-1,0),max(int(h)-1,0))); float scene=LinearDepth(g_sceneDepth.Load(int3(q,0)).r); float frag=LinearDepth(saturate(p.z)); return saturate((scene-frag)/max(softness,1.0e-3)); }'''
rep(p,particle_struct,particle_struct_new)
# particle alpha fade
rep(p,'    const float alpha = soft * input.life *\n        saturate(0.16 + 0.64 * authority + 0.20 * density);','    const float depthFade=SoftDepth(input.position,input.softnessMeters);\n    const float alpha = soft * input.life * depthFade *\n        saturate(0.16 + 0.64 * authority + 0.20 * density);')

# Splash pixel shader replace whole concise header/main alpha line.
rep(p,
'''struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float impact:TEXCOORD2; };\nstruct OitOutput { float4 accumulation:SV_Target0; float4 opticalDepth:SV_Target1; };
OitOutput main(VSOutput i) {''',
'''struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float impact:TEXCOORD2; float softnessMeters:TEXCOORD3; };\n[[vk::binding(8,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth; [[vk::binding(8,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;\nstruct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; }; [[vk::push_constant]] Push g;\nfloat LinearDepth(float d){float n=max(g.projection.z,1e-4),f=max(g.projection.w,n+1e-3);return n*f/max(f-d*(f-n),1e-5);}\nfloat SoftDepth(float4 p,float s){uint w,h;g_sceneDepth.GetDimensions(w,h);int2 q=clamp(int2(p.xy),int2(0,0),int2(max(int(w)-1,0),max(int(h)-1,0)));return saturate((LinearDepth(g_sceneDepth.Load(int3(q,0)).r)-LinearDepth(saturate(p.z)))/max(s,1e-3));}\nstruct OitOutput { float4 accumulation:SV_Target0; float4 opticalDepth:SV_Target1; };\nOitOutput main(VSOutput i) {''')
rep(p,'    float impact=saturate(i.impact/8.0); float alpha=ring*(0.35+0.55*impact);','    float impact=saturate(i.impact/8.0); float alpha=ring*(0.35+0.55*impact)*SoftDepth(i.position,i.softnessMeters);')

# Droplet pixel shader.
rep(p,
'''struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float life:TEXCOORD2; };\nstruct OitOutput { float4 accumulation:SV_Target0; float4 opticalDepth:SV_Target1; };\nOitOutput main(VSOutput i) { float r2=dot(i.uv,i.uv); if(r2>=1.0||i.life<=0.0) discard; float alpha=(1.0-smoothstep(0.2,1.0,r2))*i.life*0.82;''',
'''struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float life:TEXCOORD2; float softnessMeters:TEXCOORD3; };\n[[vk::binding(8,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth; [[vk::binding(8,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;\nstruct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; }; [[vk::push_constant]] Push g;\nfloat LinearDepth(float d){float n=max(g.projection.z,1e-4),f=max(g.projection.w,n+1e-3);return n*f/max(f-d*(f-n),1e-5);}\nfloat SoftDepth(float4 p,float s){uint w,h;g_sceneDepth.GetDimensions(w,h);int2 q=clamp(int2(p.xy),int2(0,0),int2(max(int(w)-1,0),max(int(h)-1,0)));return saturate((LinearDepth(g_sceneDepth.Load(int3(q,0)).r)-LinearDepth(saturate(p.z)))/max(s,1e-3));}\nstruct OitOutput { float4 accumulation:SV_Target0; float4 opticalDepth:SV_Target1; };\nOitOutput main(VSOutput i) { float r2=dot(i.uv,i.uv); if(r2>=1.0||i.life<=0.0) discard; float alpha=(1.0-smoothstep(0.2,1.0,r2))*i.life*0.82*SoftDepth(i.position,i.softnessMeters);''')

# Pipeline sampled depth and bind read-only depth MRT.
rep(p,'.shaderResourceBuffers = 8U,\n        .sampledTextures = 0U,','.shaderResourceBuffers = 8U,\n        .sampledTextures = 1U,')
rep(p,'.pushConstantDwords=20U,.shaderResourceBuffers=8U,.sampledTextures=0U,','.pushConstantDwords=20U,.shaderResourceBuffers=8U,.sampledTextures=1U,')
rep(p,'.pushConstantDwords=20U,.shaderResourceBuffers=8U,.sampledTextures=0U,','.pushConstantDwords=20U,.shaderResourceBuffers=8U,.sampledTextures=1U,')
rep(p,'    commands.SetRenderTargets(oitColors,&depth);','    commands.SetRenderTargetsReadOnlyDepth(oitColors,depth);')
# bind depth after each pipeline, before draw.
rep(p,'    commands.SetGraphicsConstants(constants);\n    state_.BindForGraphics(commands);\n    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::ParticleIndirectOffsetBytes);','    commands.SetGraphicsConstants(constants);\n    state_.BindForGraphics(commands);\n    commands.SetGraphicsTexture(0U,depth);\n    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::ParticleIndirectOffsetBytes);')
rep(p,'    commands.SetGraphicsConstants(constants);\n    state_.BindForGraphics(commands);\n    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::DropletIndirectOffsetBytes);','    commands.SetGraphicsConstants(constants);\n    state_.BindForGraphics(commands);\n    commands.SetGraphicsTexture(0U,depth);\n    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::DropletIndirectOffsetBytes);')
rep(p,'    commands.SetGraphicsConstants(constants);\n    state_.BindForGraphics(commands);\n    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::SplashIndirectOffsetBytes);','    commands.SetGraphicsConstants(constants);\n    state_.BindForGraphics(commands);\n    commands.SetGraphicsTexture(0U,depth);\n    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::SplashIndirectOffsetBytes);')

print('M38 soft-depth patch applied')
