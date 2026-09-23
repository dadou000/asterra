#include <orbit/volume_render/VolumeParticleRenderer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace orbit::volume_render
{
namespace
{
constexpr const char* kVertexShader = R"(
struct Particle
{
    float3 positionMeters;
    float authority;
    float3 velocityMetersPerSecond;
    float density;
    float emission;
    float ageSeconds;
    float lifetimeSeconds;
    float linearDragPerSecond;
    float radiusMeters;
    float emissionScale;
    float gravityScale;
    float restitution;
    float3 baseColor;
    uint behaviorFlags;
    float3 emissionColor;
    uint generation;
    float3 bodyCenterMeters;
    float gravitationalParameterM3PerS2;
    float3 surfaceRadiiMeters;
    float gravitySofteningMeters;
    float waterDensityRatio;
    float waterDragPerSecond;
    float waterBuoyancyScale;
    float waterSplashScale;
    uint4 bodyIdentity;
};

[[vk::binding(2, 0)]]
StructuredBuffer<Particle> g_particles : register(t2);
[[vk::binding(5, 0)]] StructuredBuffer<uint> g_particleIndices : register(t5);

struct Push
{
    float4 projection;
    float4 forward;
    float4 up;
    float4 camera;
    float4 viewport;
};

[[vk::push_constant]]
Push g;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float authority : TEXCOORD1;
    float density : TEXCOORD2;
    float emission : TEXCOORD3;
    float life : TEXCOORD4;
    float3 baseColor : TEXCOORD5;
    float3 emissionColor : TEXCOORD6;
    float emissionScale : TEXCOORD7;
    float softnessMeters : TEXCOORD8;
};

float4 Project(float3 relative)
{
    const float3 forward = normalize(g.forward.xyz);
    const float3 requestedUp = normalize(g.up.xyz);
    const float3 right = normalize(cross(forward, requestedUp));
    const float3 cameraUp = normalize(cross(right, forward));

    const float z = dot(relative, forward);
    if (z <= g.projection.z || z >= g.projection.w)
    {
        return float4(2.0, 2.0, 1.0, 1.0);
    }

    const float x = dot(relative, right);
    const float y = dot(relative, cameraUp);

    const float n=max(g.projection.z,1.0e-4), f=max(g.projection.w,n+1.0e-3);
    const float clipZ=z*f/(f-n)-f*n/(f-n);
    return float4(
        x / (max(g.projection.x, 0.001) * max(g.projection.y, 0.001)),
        -y / max(g.projection.y, 0.001),
        clipZ,
        z);
}

VSOutput main(uint vertexId : SV_VertexID)
{
    static const float2 corners[6] =
    {
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0,  1.0)
    };

    const uint particleIndex = g_particleIndices[vertexId / 6u];
    const uint cornerIndex = vertexId % 6u;
    const Particle particle = g_particles[particleIndex];
    const uint currentGeneration = asuint(g.viewport.w);

    VSOutput output;
    if (particle.generation != currentGeneration ||
        particle.lifetimeSeconds <= 0.0 ||
        particle.ageSeconds >= particle.lifetimeSeconds)
    {
        output.position = float4(2.0, 2.0, 1.0, 1.0);
        output.uv = 0.0;
        output.authority = 0.0;
        output.density = 0.0;
        output.emission = 0.0;
        output.life = 0.0;
        output.baseColor = 0.0;
        output.emissionColor = 0.0;
        output.emissionScale = 0.0;
        output.softnessMeters = 0.0;
        return output;
    }

    const float4 center = Project(
        particle.positionMeters - g.camera.xyz);

    if (center.w <= 0.0)
    {
        output.position = center;
        output.uv = 0.0;
        output.authority = 0.0;
        output.density = 0.0;
        output.emission = 0.0;
        output.life = 0.0;
        output.baseColor = 0.0;
        output.emissionColor = 0.0;
        output.emissionScale = 0.0;
        output.softnessMeters = 0.0;
        return output;
    }

    const float2 ndcPerPixel = float2(
        2.0 / max(g.viewport.x, 1.0),
        2.0 / max(g.viewport.y, 1.0));
    const float2 corner = corners[cornerIndex];
    const float projectedRadiusPixels =
        max(particle.radiusMeters, 0.001) /
        max(center.w * max(g.projection.y, 0.001), 0.001) *
        max(g.viewport.y, 1.0) * 0.5;
    const float radiusPixels = max(projectedRadiusPixels, max(g.viewport.z, 0.5));

    output.position = center;
    output.position.xy += corner * ndcPerPixel * radiusPixels * center.w;
    output.uv = corner;
    output.authority = max(particle.authority, 0.0);
    output.density = max(particle.density, 0.0);
    output.emission = max(particle.emission, 0.0);
    output.life = saturate(
        1.0 - particle.ageSeconds /
            max(particle.lifetimeSeconds, 0.001));
    output.baseColor = max(particle.baseColor, 0.0);
    output.emissionColor = max(particle.emissionColor, 0.0);
    output.emissionScale = max(particle.emissionScale, 0.0);
    output.softnessMeters = max(particle.radiusMeters * 2.0, 0.05);
    return output;
}
)";


constexpr const char* kSplashVertexShader = R"(
struct SplashState { float3 positionMeters; float baseScaleMeters; float3 normal; float expansionMetersPerSecond; float3 tint; float impactSpeedMetersPerSecond; float ageSeconds; float lifetimeSeconds; uint generation; uint reserved; };
[[vk::binding(3, 0)]] StructuredBuffer<SplashState> g_splashes : register(t3);
[[vk::binding(6, 0)]] StructuredBuffer<uint> g_splashIndices : register(t6);
struct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; };
[[vk::push_constant]] Push g;
struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float impact:TEXCOORD2; float softnessMeters:TEXCOORD3; };
float4 Project(float3 relative) {
    float3 forward=normalize(g.forward.xyz); float3 requestedUp=normalize(g.up.xyz);
    float3 right=normalize(cross(forward,requestedUp)); float3 cameraUp=normalize(cross(right,forward));
    float z=dot(relative,forward); if(z<=g.projection.z||z>=g.projection.w) return float4(2,2,1,1);
    float x=dot(relative,right), y=dot(relative,cameraUp);
    float n=max(g.projection.z,1.0e-4),f=max(g.projection.w,n+1.0e-3); float clipZ=z*f/(f-n)-f*n/(f-n); return float4(x/(max(g.projection.x,0.001)*max(g.projection.y,0.001)),-y/max(g.projection.y,0.001),clipZ,z);
}
VSOutput main(uint vertexId:SV_VertexID) {
    static const float2 corners[6]={float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,-1),float2(1,1),float2(-1,1)};
    uint eventIndex=g_splashIndices[vertexId/6u], cornerIndex=vertexId%6u; SplashState e=g_splashes[eventIndex]; VSOutput o;
    if(e.generation!=asuint(g.viewport.w)||e.baseScaleMeters<=0.0||e.lifetimeSeconds<=0.0||e.ageSeconds>=e.lifetimeSeconds){o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.impact=0;o.softnessMeters=0;return o;}
    float4 center=Project(e.positionMeters-g.camera.xyz); if(center.w<=0.0){o.position=center;o.uv=0;o.tint=0;o.impact=0;o.softnessMeters=0;return o;}
    float2 ndc=float2(2.0/max(g.viewport.x,1.0),2.0/max(g.viewport.y,1.0));
    float life=saturate(1.0-e.ageSeconds/max(e.lifetimeSeconds,0.001));
    float radius=max(e.baseScaleMeters+e.expansionMetersPerSecond*e.ageSeconds,0.01);
    float pixels=radius/max(center.w*max(g.projection.y,0.001),0.001)*max(g.viewport.y,1.0)*0.5;
    pixels=max(pixels,max(g.viewport.z,0.5)); o.position=center; o.position.xy+=corners[cornerIndex]*ndc*pixels*center.w;
    o.uv=corners[cornerIndex]; o.tint=max(e.tint,0.0)*life; o.impact=max(e.impactSpeedMetersPerSecond,0.0)*life; o.softnessMeters=max(radius*0.25,0.03); return o;
}
)";
constexpr const char* kSplashPixelShader = R"(
struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float impact:TEXCOORD2; float softnessMeters:TEXCOORD3; };
[[vk::binding(8,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth; [[vk::binding(8,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;
struct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; }; [[vk::push_constant]] Push g;
float LinearDepth(float d){float n=max(g.projection.z,1e-4),f=max(g.projection.w,n+1e-3);return n*f/max(f-d*(f-n),1e-5);}
float SoftDepth(float4 p,float s){uint w,h;g_sceneDepth.GetDimensions(w,h);int2 q=clamp(int2(p.xy),int2(0,0),int2(max(int(w)-1,0),max(int(h)-1,0)));return saturate((LinearDepth(g_sceneDepth.Load(int3(q,0)).r)-LinearDepth(saturate(p.z)))/max(s,1e-3));}
struct OitOutput { float4 accumulation:SV_Target0; float4 opticalDepth:SV_Target1; };
OitOutput main(VSOutput i) {
    float r=length(i.uv); if(r>=1.0||r<0.42) discard;
    float ring=(1.0-smoothstep(0.42,0.62,r))*smoothstep(0.42,0.52,r);
    float impact=saturate(i.impact/8.0); float alpha=ring*(0.35+0.55*impact)*SoftDepth(i.position,i.softnessMeters);
    float3 foam=lerp(float3(0.72,0.84,0.90),float3(1.0,1.0,1.0),impact)*i.tint;
    OitOutput o; float optical=-log(max(1.0-saturate(alpha),1.0e-4));
    o.accumulation=float4(foam*alpha,alpha); o.opticalDepth=float4(optical,0,0,0); return o;
}
)";

constexpr const char* kDropletVertexShader = R"(
struct Droplet { float3 positionMeters; float radiusMeters; float3 velocityMetersPerSecond; float ageSeconds; float3 tint; float lifetimeSeconds; float3 bodyCenterMeters; float gravitationalParameterM3PerS2; float3 surfaceRadiiMeters; float gravitySofteningMeters; uint4 bodyIdentity; uint generation; uint flags; uint2 reserved; };
[[vk::binding(4,0)]] StructuredBuffer<Droplet> g_droplets : register(t4);
[[vk::binding(7, 0)]] StructuredBuffer<uint> g_dropletIndices : register(t7);
struct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; }; [[vk::push_constant]] Push g;
struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float life:TEXCOORD2; float softnessMeters:TEXCOORD3; };
float4 Project(float3 relative){ float3 fw=normalize(g.forward.xyz),u=normalize(g.up.xyz),r=normalize(cross(fw,u)),cu=normalize(cross(r,fw)); float z=dot(relative,fw); if(z<=g.projection.z||z>=g.projection.w)return float4(2,2,1,1); float n=max(g.projection.z,1.0e-4),f=max(g.projection.w,n+1.0e-3); float clipZ=z*f/(f-n)-f*n/(f-n); return float4(dot(relative,r)/(max(g.projection.x,0.001)*max(g.projection.y,0.001)),-dot(relative,cu)/max(g.projection.y,0.001),clipZ,z); }
VSOutput main(uint vertexId:SV_VertexID){ static const float2 corners[6]={float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,-1),float2(1,1),float2(-1,1)}; uint i=g_dropletIndices[vertexId/6u],c=vertexId%6u; Droplet d=g_droplets[i]; VSOutput o; if(d.generation!=asuint(g.viewport.w)||d.lifetimeSeconds<=0.0||d.ageSeconds>=d.lifetimeSeconds){o.position=float4(2,2,1,1);o.uv=0;o.tint=0;o.life=0;o.softnessMeters=0;return o;} float4 center=Project(d.positionMeters-g.camera.xyz); if(center.w<=0){o.position=center;o.uv=0;o.tint=0;o.life=0;o.softnessMeters=0;return o;} float2 ndc=float2(2.0/max(g.viewport.x,1.0),2.0/max(g.viewport.y,1.0)); float px=max(d.radiusMeters,0.002)/max(center.w*max(g.projection.y,0.001),0.001)*max(g.viewport.y,1.0)*0.5; px=max(px,0.75); o.position=center;o.position.xy+=corners[c]*ndc*px*center.w;o.uv=corners[c];o.tint=max(d.tint,0.0);o.life=saturate(1.0-d.ageSeconds/max(d.lifetimeSeconds,0.001));o.softnessMeters=max(d.radiusMeters*4.0,0.02);return o; }
)";
constexpr const char* kDropletPixelShader = R"(
struct VSOutput { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 tint:TEXCOORD1; float life:TEXCOORD2; float softnessMeters:TEXCOORD3; };
[[vk::binding(8,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth; [[vk::binding(8,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;
struct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; }; [[vk::push_constant]] Push g;
float LinearDepth(float d){float n=max(g.projection.z,1e-4),f=max(g.projection.w,n+1e-3);return n*f/max(f-d*(f-n),1e-5);}
float SoftDepth(float4 p,float s){uint w,h;g_sceneDepth.GetDimensions(w,h);int2 q=clamp(int2(p.xy),int2(0,0),int2(max(int(w)-1,0),max(int(h)-1,0)));return saturate((LinearDepth(g_sceneDepth.Load(int3(q,0)).r)-LinearDepth(saturate(p.z)))/max(s,1e-3));}
struct OitOutput { float4 accumulation:SV_Target0; float4 opticalDepth:SV_Target1; };
OitOutput main(VSOutput i) { float r2=dot(i.uv,i.uv); if(r2>=1.0||i.life<=0.0) discard; float alpha=(1.0-smoothstep(0.2,1.0,r2))*i.life*0.82*SoftDepth(i.position,i.softnessMeters); float3 c=lerp(float3(0.70,0.84,0.94),float3(1,1,1),0.65)*i.tint; OitOutput o; float optical=-log(max(1.0-saturate(alpha),1.0e-4)); o.accumulation=float4(c*alpha,alpha); o.opticalDepth=float4(optical,0,0,0); return o; }
)";

constexpr const char* kPixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float authority : TEXCOORD1;
    float density : TEXCOORD2;
    float emission : TEXCOORD3;
    float life : TEXCOORD4;
    float3 baseColor : TEXCOORD5;
    float3 emissionColor : TEXCOORD6;
    float emissionScale : TEXCOORD7;    float softnessMeters : TEXCOORD8;
};
[[vk::binding(8,0)]] [[vk::combinedImageSampler]] Texture2D g_sceneDepth;
[[vk::binding(8,0)]] [[vk::combinedImageSampler]] SamplerState g_sceneDepthSampler;
struct Push { float4 projection; float4 forward; float4 up; float4 camera; float4 viewport; }; [[vk::push_constant]] Push g;
float LinearDepth(float d){float n=max(g.projection.z,1e-4),f=max(g.projection.w,n+1e-3);return n*f/max(f-d*(f-n),1e-5);}
float SoftDepth(float4 p,float s){uint w,h;g_sceneDepth.GetDimensions(w,h);int2 q=clamp(int2(p.xy),int2(0,0),int2(max(int(w)-1,0),max(int(h)-1,0)));return saturate((LinearDepth(g_sceneDepth.Load(int3(q,0)).r)-LinearDepth(saturate(p.z)))/max(s,1e-3));}

struct OitOutput { float4 accumulation : SV_Target0; float4 opticalDepth : SV_Target1; };
OitOutput main(VSOutput input)
{
    const float radius2 = dot(input.uv, input.uv);
    if (radius2 >= 1.0 || input.life <= 0.0)
    {
        discard;
    }

    const float soft = 1.0 - smoothstep(0.30, 1.0, radius2);
    const float density = saturate(input.density);
    const float emission = max(input.emission, 0.0);
    const float authority = saturate(input.authority);

    const float3 densityColor = input.baseColor * lerp(0.72, 1.0, density);
    const float3 emissiveColor = input.emissionColor * emission * input.emissionScale;
    const float3 color = densityColor + emissiveColor;
    const float depthFade=SoftDepth(input.position,input.softnessMeters);
    const float alpha = soft * input.life * depthFade *
        saturate(0.16 + 0.64 * authority + 0.20 * density);

    OitOutput output;
    const float opticalDepth = -log(max(1.0 - saturate(alpha), 1.0e-4));
    output.accumulation = float4(color * alpha, alpha);
    output.opticalDepth = float4(opticalDepth, 0.0, 0.0, 0.0);
    return output;
}
)";

constexpr const char* kOitCompositeVertexShader = R"(
struct O { float4 position:SV_Position; float2 uv:TEXCOORD0; };
O main(uint id:SV_VertexID){ static const float2 p[6]={float2(-1,-1),float2(-1,1),float2(1,-1),float2(1,-1),float2(-1,1),float2(1,1)}; static const float2 u[6]={float2(0,1),float2(0,0),float2(1,1),float2(1,1),float2(0,0),float2(1,0)}; O o;o.position=float4(p[id],0,1);o.uv=u[id];return o; }
)";
constexpr const char* kOitCompositePixelShader = R"(
struct O { float4 position:SV_Position; float2 uv:TEXCOORD0; };
[[vk::binding(0,0)]][[vk::combinedImageSampler]] Texture2D g_accum;
[[vk::binding(0,0)]][[vk::combinedImageSampler]] SamplerState g_accumSampler;
[[vk::binding(1,0)]][[vk::combinedImageSampler]] Texture2D g_optical;
[[vk::binding(1,0)]][[vk::combinedImageSampler]] SamplerState g_opticalSampler;
float4 main(O i):SV_Target0 { float4 a=g_accum.Sample(g_accumSampler,i.uv); float optical=max(g_optical.Sample(g_opticalSampler,i.uv).r,0.0); if(a.a<=1.0e-6||optical<=1.0e-6) discard; float3 color=a.rgb/max(a.a,1.0e-6); float alpha=1.0-exp(-min(optical,20.0)); return float4(color,alpha); }
)";
}

VolumeParticleRenderer::VolumeParticleRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler,
    const u32 framesInFlight)
    : device_(&device),
      framesInFlight_(framesInFlight),
      state_(device, compiler, framesInFlight)
{
    const auto vertex = compiler.Compile({
        .source = kVertexShader,
        .entryPoint = "main",
        .stage = shader::Stage::Vertex,
        .debug = false
    });
    const auto pixel = compiler.Compile({
        .source = kPixelShader,
        .entryPoint = "main",
        .stage = shader::Stage::Pixel,
        .debug = false
    });

    const auto splashVertex = compiler.Compile({.source=kSplashVertexShader,.entryPoint="main",.stage=shader::Stage::Vertex,.debug=false});
    const auto splashPixel = compiler.Compile({.source=kSplashPixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});
    const auto dropletVertex = compiler.Compile({.source=kDropletVertexShader,.entryPoint="main",.stage=shader::Stage::Vertex,.debug=false});
    const auto dropletPixel = compiler.Compile({.source=kDropletPixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});
    const auto oitCompositeVertex=compiler.Compile({.source=kOitCompositeVertexShader,.entryPoint="main",.stage=shader::Stage::Vertex,.debug=false});
    const auto oitCompositePixel=compiler.Compile({.source=kOitCompositePixelShader,.entryPoint="main",.stage=shader::Stage::Pixel,.debug=false});
    if (vertex.bytecode.empty() || pixel.bytecode.empty() || splashVertex.bytecode.empty() || splashPixel.bytecode.empty() || dropletVertex.bytecode.empty() || dropletPixel.bytecode.empty() || oitCompositeVertex.bytecode.empty() || oitCompositePixel.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile M38 persistent particle shaders.");
    }

    pipeline_ = device.CreateGraphicsPipeline({
        .vertexShader = {
            .data = vertex.bytecode.data(),
            .size = vertex.bytecode.size()
        },
        .pixelShader = {
            .data = pixel.bytecode.data(),
            .size = pixel.bytecode.size()
        },
        .vertexAttributes = {},
        .vertexStrideBytes = 0U,
        .pushConstantDwords = 20U,
        .shaderResourceBuffers = 8U,
        .sampledTextures = 1U,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Additive,
        .depthCompare = rhi::DepthCompare::LessEqual,
        .depthTest = true,
        .depthWrite = false,
        .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float, rhi::TextureFormat::R16_Float},
        .colorAttachmentCount = 2U
    });

    splashPipeline_ = device.CreateGraphicsPipeline({
        .vertexShader={.data=splashVertex.bytecode.data(),.size=splashVertex.bytecode.size()},
        .pixelShader={.data=splashPixel.bytecode.data(),.size=splashPixel.bytecode.size()},
        .vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=20U,.shaderResourceBuffers=8U,.sampledTextures=1U,
        .topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,
        .blendMode=rhi::BlendMode::Additive,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=true,.depthWrite=false,
        .colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float,rhi::TextureFormat::R16_Float},.colorAttachmentCount=2U
    });
    dropletPipeline_=device.CreateGraphicsPipeline({.vertexShader={.data=dropletVertex.bytecode.data(),.size=dropletVertex.bytecode.size()},.pixelShader={.data=dropletPixel.bytecode.data(),.size=dropletPixel.bytecode.size()},.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=20U,.shaderResourceBuffers=8U,.sampledTextures=1U,.topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,.blendMode=rhi::BlendMode::Additive,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=true,.depthWrite=false,.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float,rhi::TextureFormat::R16_Float},.colorAttachmentCount=2U});
    oitCompositePipeline_=device.CreateGraphicsPipeline({.vertexShader={.data=oitCompositeVertex.bytecode.data(),.size=oitCompositeVertex.bytecode.size()},.pixelShader={.data=oitCompositePixel.bytecode.data(),.size=oitCompositePixel.bytecode.size()},.vertexAttributes={},.vertexStrideBytes=0U,.pushConstantDwords=0U,.shaderResourceBuffers=0U,.sampledTextures=2U,.topology=rhi::PrimitiveTopology::TriangleList,.fillMode=rhi::FillMode::Solid,.cullMode=rhi::CullMode::None,.blendMode=rhi::BlendMode::Alpha,.depthCompare=rhi::DepthCompare::LessEqual,.depthTest=false,.depthWrite=false,.colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},.colorAttachmentCount=1U});
}

VolumeParticleRenderer::OitTargets& VolumeParticleRenderer::EnsureOitTargets(const u32 width,const u32 height,const u32 frameIndex)
{
    if(frameIndex>=framesInFlight_) throw std::out_of_range("Orbit M38 OIT frame index exceeds frames in flight.");
    const u64 key=(static_cast<u64>(width)<<32U)|static_cast<u64>(height);
    auto& slots=oitTargets_[key];
    if(slots.empty()) slots.resize(framesInFlight_);
    auto& target=slots[frameIndex];
    if(target.accumulation==nullptr){
        target.accumulation=device_->CreateTexture({.width=width,.height=height,.format=rhi::TextureFormat::RGBA16_Float,.initialState=rhi::ResourceState::ShaderResource});
        target.opticalDepth=device_->CreateTexture({.width=width,.height=height,.format=rhi::TextureFormat::R16_Float,.initialState=rhi::ResourceState::ShaderResource});
        target.accumulationState=rhi::ResourceState::ShaderResource; target.opticalDepthState=rhi::ResourceState::ShaderResource;
    }
    return target;
}

void VolumeParticleRenderer::SetSpawns(
    const std::span<const VolumeParticleGpuSpawn> spawns)
{
    state_.SetSpawns(spawns);
}

void VolumeParticleRenderer::Advance(
    rhi::CommandList& commands,
    const u32 frameIndex,
    const f64 deltaSeconds,
    const math::Double3 previousOriginMeters,
    const math::Double3 newOriginMeters,
    const VolumeParticleSimulationSettings settings)
{
    state_.Advance(
        commands,
        frameIndex,
        deltaSeconds,
        previousOriginMeters,
        newOriginMeters,
        settings);
}


void VolumeParticleRenderer::UpdateTerrainCollisionPages(
    const std::array<u32, 4U>& bodyIdentity,
    const std::span<const VolumeParticleTerrainCollisionPage> pages)
{
    terrainCollisionPages_.erase(
        std::remove_if(
            terrainCollisionPages_.begin(),
            terrainCollisionPages_.end(),
            [&bodyIdentity](const auto& page)
            {
                return page.bodyIdentity == bodyIdentity;
            }),
        terrainCollisionPages_.end());

    for (const auto& page : pages)
    {
        if (page.IsValid())
        {
            terrainCollisionPages_.push_back(page);
        }
    }

    std::stable_sort(
        terrainCollisionPages_.begin(),
        terrainCollisionPages_.end(),
        [](const auto& a, const auto& b)
        {
            return a.level > b.level;
        });
}

std::vector<VolumeParticleTerrainCollisionPage>
VolumeParticleRenderer::TerrainCollisionPagesSnapshot() const
{
    return terrainCollisionPages_;
}

void VolumeParticleRenderer::ApplyTerrainCollision(
    rhi::CommandList& commands,
    const std::span<const VolumeParticleTerrainCollisionPage> pages,
    const f64 deltaSeconds)
{
    state_.ApplyTerrainCollision(commands, pages, deltaSeconds);
}

void VolumeParticleRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& sceneColor,
    rhi::Texture& depth,
    const u32 width,
    const u32 height,
    const render_view::CameraState& camera,
    const math::Double3 cameraPositionRelativeToPresentationOriginMeters,
    const u32 frameIndex,
    const f32 radiusPixels)
{
    if (state_.Generation() == 0U || width == 0U || height == 0U)
    {
        return;
    }

    const f32 tanHalfFov = std::tan(camera.verticalFovRadians * 0.5F);
    const auto bits = [](const f32 value)
    {
        return std::bit_cast<u32>(value);
    };

    std::array<u32, 20> constants{};
    constants[0] = bits(
        static_cast<f32>(width) /
        static_cast<f32>(height));
    constants[1] = bits(tanHalfFov);
    constants[2] = bits(camera.nearPlaneMeters);
    constants[3] = bits(camera.farPlaneMeters);

    constants[4] = bits(camera.forward.x);
    constants[5] = bits(camera.forward.y);
    constants[6] = bits(camera.forward.z);

    constants[8] = bits(camera.up.x);
    constants[9] = bits(camera.up.y);
    constants[10] = bits(camera.up.z);

    constants[12] = bits(static_cast<f32>(
        cameraPositionRelativeToPresentationOriginMeters.x));
    constants[13] = bits(static_cast<f32>(
        cameraPositionRelativeToPresentationOriginMeters.y));
    constants[14] = bits(static_cast<f32>(
        cameraPositionRelativeToPresentationOriginMeters.z));

    constants[16] = bits(static_cast<f32>(width));
    constants[17] = bits(static_cast<f32>(height));
    constants[18] = bits(
        std::isfinite(radiusPixels)
            ? std::max(radiusPixels, 0.5F)
            : 3.0F);
    constants[19] = state_.Generation();

    state_.BuildVisibleDrawLists(
        commands,
        {
            static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.x),
            static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.y),
            static_cast<f32>(cameraPositionRelativeToPresentationOriginMeters.z)
        },
        camera.forward,
        camera.up,
        static_cast<f32>(width) / static_cast<f32>(height),
        tanHalfFov,
        camera.nearPlaneMeters,
        camera.farPlaneMeters);

    auto& oit=EnsureOitTargets(width,height,frameIndex);
    if(oit.accumulationState!=rhi::ResourceState::RenderTarget){commands.Transition(*oit.accumulation,oit.accumulationState,rhi::ResourceState::RenderTarget);oit.accumulationState=rhi::ResourceState::RenderTarget;}
    if(oit.opticalDepthState!=rhi::ResourceState::RenderTarget){commands.Transition(*oit.opticalDepth,oit.opticalDepthState,rhi::ResourceState::RenderTarget);oit.opticalDepthState=rhi::ResourceState::RenderTarget;}
    commands.ClearColorTarget(*oit.accumulation,{0,0,0,0});
    commands.ClearColorTarget(*oit.opticalDepth,{0,0,0,0});
    std::array<rhi::Texture*,2U> oitColors{oit.accumulation.get(),oit.opticalDepth.get()};
    commands.SetRenderTargetsReadOnlyDepth(oitColors,depth);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<f32>(width),
        .height = static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    });
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right = static_cast<i32>(width),
        .bottom = static_cast<i32>(height)
    });
    commands.SetGraphicsPipeline(*pipeline_);
    commands.SetGraphicsConstants(constants);
    state_.BindForGraphics(commands);
    commands.SetGraphicsTexture(0U,depth);
    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::ParticleIndirectOffsetBytes);
    constants[19] = state_.DropletGeneration();
    commands.SetGraphicsPipeline(*dropletPipeline_);
    commands.SetGraphicsConstants(constants);
    state_.BindForGraphics(commands);
    commands.SetGraphicsTexture(0U,depth);
    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::DropletIndirectOffsetBytes);
    constants[19] = state_.SplashGeneration();
    commands.SetGraphicsPipeline(*splashPipeline_);
    commands.SetGraphicsConstants(constants);
    state_.BindForGraphics(commands);
    commands.SetGraphicsTexture(0U,depth);
    commands.DrawIndirect(state_.IndirectDrawArguments(), VolumeParticleGpuState::SplashIndirectOffsetBytes);

    commands.Transition(*oit.accumulation,oit.accumulationState,rhi::ResourceState::ShaderResource); oit.accumulationState=rhi::ResourceState::ShaderResource;
    commands.Transition(*oit.opticalDepth,oit.opticalDepthState,rhi::ResourceState::ShaderResource); oit.opticalDepthState=rhi::ResourceState::ShaderResource;
    commands.SetRenderTarget(sceneColor);
    commands.SetGraphicsPipeline(*oitCompositePipeline_);
    commands.SetGraphicsTexture(0U,*oit.accumulation);
    commands.SetGraphicsTexture(1U,*oit.opticalDepth);
    commands.Draw(6U);
}

void VolumeParticleRenderer::Reset() noexcept
{
    state_.Reset();
    terrainCollisionPages_.clear();
}

u32 VolumeParticleRenderer::Generation() const noexcept
{
    return state_.Generation();
}

u32 VolumeParticleRenderer::SubmittedSpawnCount() const noexcept
{
    return state_.SubmittedSpawnCount();
}
} // namespace orbit::volume_render
