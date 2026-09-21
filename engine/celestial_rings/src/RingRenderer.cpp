#include <orbit/celestial_rings/RingSystem.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace orbit::celestial_rings
{
GpuRingMeshProduct::GpuRingMeshProduct(
    rhi::Device& device,
    const RingMeshProduct& product)
    : indexCount_(static_cast<u32>(product.indices.size())),
      referenceRadiusMeters_(product.referenceRadiusMeters),
      fingerprint_(product.fingerprint)
{
    if(product.vertices.empty() || product.indices.empty() ||
       product.referenceRadiusMeters<=0.0)
        throw std::invalid_argument("GPU ring mesh requires a valid product.");

    vertices_=device.CreateBuffer({
        .sizeBytes=static_cast<u64>(product.vertices.size()*sizeof(RingVertex)),
        .usage=rhi::BufferUsage::Vertex,
        .memory=rhi::MemoryUsage::HostVisible,
        .initialState=rhi::ResourceState::VertexOrConstantBuffer
    });
    indices_=device.CreateBuffer({
        .sizeBytes=static_cast<u64>(product.indices.size()*sizeof(u32)),
        .usage=rhi::BufferUsage::Index,
        .memory=rhi::MemoryUsage::HostVisible,
        .initialState=rhi::ResourceState::IndexBuffer
    });
    if(!vertices_ || !indices_)
        throw std::runtime_error("Failed to allocate ring GPU buffers.");
    std::memcpy(vertices_->Map(),product.vertices.data(),
        product.vertices.size()*sizeof(RingVertex));
    vertices_->Unmap();
    std::memcpy(indices_->Map(),product.indices.data(),
        product.indices.size()*sizeof(u32));
    indices_->Unmap();
}
rhi::Buffer& GpuRingMeshProduct::VertexBuffer() noexcept{return *vertices_;}
rhi::Buffer& GpuRingMeshProduct::IndexBuffer() noexcept{return *indices_;}
u32 GpuRingMeshProduct::IndexCount() const noexcept{return indexCount_;}
f64 GpuRingMeshProduct::ReferenceRadiusMeters() const noexcept{return referenceRadiusMeters_;}
u64 GpuRingMeshProduct::Fingerprint() const noexcept{return fingerprint_;}

namespace
{
constexpr const char* kVs=R"(
struct VSIn
{
    float3 position : POSITION;
    float3 color : COLOR0;
    float opticalDepth : TEXCOORD0;
    float singleScatteringAlbedo : TEXCOORD1;
    float anisotropy : TEXCOORD2;
};
struct Constants
{
    float4 cameraAndAspect;
    float4 forwardAndTanHalfFov;
    float4 upAndScale;
    float4 lightAndScale;
    float4 ringPlane;
};
[[vk::push_constant]] Constants g;
struct VSOut
{
    float4 position : SV_Position;
    float3 bodyPosition : TEXCOORD0;
    float3 color : COLOR0;
    float opticalDepth : TEXCOORD1;
    float singleScatteringAlbedo : TEXCOORD2;
    float anisotropy : TEXCOORD3;
    float3 viewDirection : TEXCOORD4;
};
VSOut main(VSIn i)
{
    const float3 forward=normalize(g.forwardAndTanHalfFov.xyz);
    const float3 requestedUp=normalize(g.upAndScale.xyz);
    const float3 right=normalize(cross(forward,requestedUp));
    const float3 up=normalize(cross(right,forward));
    const float3 camera=g.cameraAndAspect.xyz;
    const float3 world=i.position*g.upAndScale.w;
    const float3 relative=world-camera;
    const float z=dot(relative,forward);
    const float x=dot(relative,right);
    const float y=dot(relative,up);
    const float tanHalf=max(g.forwardAndTanHalfFov.w,0.001);
    const float aspect=max(g.cameraAndAspect.w,0.001);
    VSOut o;
    o.position=float4(x/(tanHalf*aspect),y/tanHalf,z*0.5,z);
    o.bodyPosition=i.position;
    o.color=i.color;
    o.opticalDepth=max(i.opticalDepth,0.0);
    o.singleScatteringAlbedo=saturate(i.singleScatteringAlbedo);
    o.anisotropy=clamp(i.anisotropy,-0.999,0.999);
    o.viewDirection=normalize(camera-world);
    return o;
}
)";
constexpr const char* kPs=R"(
struct Constants
{
    float4 cameraAndAspect;
    float4 forwardAndTanHalfFov;
    float4 upAndScale;
    float4 lightAndScale;
    float4 ringPlane;
};
[[vk::push_constant]] Constants g;
struct VSOut
{
    float4 position : SV_Position;
    float3 bodyPosition : TEXCOORD0;
    float3 color : COLOR0;
    float opticalDepth : TEXCOORD1;
    float singleScatteringAlbedo : TEXCOORD2;
    float anisotropy : TEXCOORD3;
    float3 viewDirection : TEXCOORD4;
};
float RaySphereBlocked(float3 ro,float3 rd)
{
    const float b=2.0*dot(ro,rd);
    const float c=dot(ro,ro)-1.0;
    const float d=b*b-4.0*c;
    if(d<0.0) return 0.0;
    const float q=sqrt(d);
    const float t0=(-b-q)*0.5;
    const float t1=(-b+q)*0.5;
    return (t0>1e-5 || t1>1e-5)?1.0:0.0;
}
float PhaseHG(float c,float g)
{
    const float d=max(1.0+g*g-2.0*g*c,1e-5);
    return (1.0-g*g)/(12.5663706*pow(d,1.5));
}
float4 main(VSOut i):SV_Target0
{
    const float3 n=normalize(g.ringPlane.xyz);
    const float3 l=normalize(g.lightAndScale.xyz);
    const float3 v=normalize(i.viewDirection);

    // Back-ring fragments hidden by the solid body never reach the final color.
    const float3 camera=g.cameraAndAspect.xyz;
    const float3 toPoint=i.bodyPosition-camera;
    const float pointDistance=length(toPoint);
    const float3 ray=toPoint/max(pointDistance,1e-6);
    const float b=2.0*dot(camera,ray);
    const float cc=dot(camera,camera)-1.0;
    const float disc=b*b-4.0*cc;
    if(disc>=0.0)
    {
        const float nearT=(-b-sqrt(disc))*0.5;
        if(nearT>0.0 && nearT<pointDistance)
            discard;
    }

    const float muV=max(abs(dot(n,v)),1e-4);
    const float muL=max(abs(dot(n,l)),1e-4);
    const float viewTransmission=exp(-i.opticalDepth/muV);
    const float opacity=1.0-viewTransmission;
    const float intercepted=1.0-exp(-i.opticalDepth/muL);

    float bodyLight=1.0;
    if(g.ringPlane.w>0.5)
        bodyLight=1.0-RaySphereBlocked(i.bodyPosition,l);

    const float phase=PhaseHG(dot(-l,v),i.anisotropy);
    const float scattering=
        intercepted*i.singleScatteringAlbedo*phase*
        max(g.lightAndScale.w,0.0)*bodyLight;

    const float3 color=
        i.color*(0.04*opacity+scattering*6.2831853);

    return float4(color,saturate(opacity));
}
)";
}

RingRenderer::RingRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const auto vs=compiler.Compile({
        .source=kVs,.entryPoint="main",
        .stage=shader::Stage::Vertex,.debug=false});
    const auto ps=compiler.Compile({
        .source=kPs,.entryPoint="main",
        .stage=shader::Stage::Pixel,.debug=false});
    static constexpr std::array<rhi::VertexAttribute,5> attrs{{
        {.location=0,.format=rhi::VertexFormat::Float3,
         .offsetBytes=static_cast<u32>(offsetof(RingVertex,positionNormalized))},
        {.location=1,.format=rhi::VertexFormat::Float3,
         .offsetBytes=static_cast<u32>(offsetof(RingVertex,colorLinear))},
        {.location=2,.format=rhi::VertexFormat::Float,
         .offsetBytes=static_cast<u32>(offsetof(RingVertex,opticalDepth))},
        {.location=3,.format=rhi::VertexFormat::Float,
         .offsetBytes=static_cast<u32>(offsetof(RingVertex,singleScatteringAlbedo))},
        {.location=4,.format=rhi::VertexFormat::Float,
         .offsetBytes=static_cast<u32>(offsetof(RingVertex,anisotropy))}
    }};
    pipeline_=device.CreateGraphicsPipeline({
        .vertexShader={.data=vs.bytecode.data(),.size=vs.bytecode.size()},
        .pixelShader={.data=ps.bytecode.data(),.size=ps.bytecode.size()},
        .vertexAttributes=attrs,
        .vertexStrideBytes=sizeof(RingVertex),
        .pushConstantDwords=20,
        .topology=rhi::PrimitiveTopology::TriangleList,
        .fillMode=rhi::FillMode::Solid,
        .cullMode=rhi::CullMode::None,
        .blendMode=rhi::BlendMode::Alpha,
        .depthTest=false,
        .depthWrite=false,
        .colorAttachmentFormats={rhi::TextureFormat::RGBA16_Float},
        .colorAttachmentCount=1U
    });
}

void RingRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    GpuRingMeshProduct& rings,
    const render_view::CameraState& camera,
    math::Float3 planeNormal,
    const bool receiveBodyShadow,
    const RingRenderLighting& lighting)
{
    if(width==0U || height==0U || rings.IndexCount()==0U)
        return;
    const f64 radius=std::max(rings.ReferenceRadiusMeters(),1.0);
    const auto bits=[](f32 v){return std::bit_cast<u32>(v);};
    const std::array<u32,20> constants{
        bits(static_cast<f32>(camera.localPositionMeters.x/radius)),
        bits(static_cast<f32>(camera.localPositionMeters.y/radius)),
        bits(static_cast<f32>(camera.localPositionMeters.z/radius)),
        bits(static_cast<f32>(width)/height),
        bits(camera.forward.x),bits(camera.forward.y),bits(camera.forward.z),
        bits(std::tan(camera.verticalFovRadians*0.5F)),
        bits(camera.up.x),bits(camera.up.y),bits(camera.up.z),bits(1.0F),
        bits(lighting.directionBody.x),bits(lighting.directionBody.y),
        bits(lighting.directionBody.z),bits(std::max(lighting.irradianceScale,0.0F)),
        bits(planeNormal.x),bits(planeNormal.y),bits(planeNormal.z),
        bits(receiveBodyShadow?1.0F:0.0F)
    };
    commands.SetRenderTarget(target);
    commands.SetViewport({.x=0,.y=0,.width=static_cast<f32>(width),
        .height=static_cast<f32>(height),.minDepth=0,.maxDepth=1});
    commands.SetScissor({.left=0,.top=0,.right=static_cast<i32>(width),
        .bottom=static_cast<i32>(height)});
    commands.SetGraphicsPipeline(*pipeline_);
    commands.SetGraphicsConstants(constants);
    commands.SetVertexBuffer(rings.VertexBuffer(),sizeof(RingVertex));
    commands.SetIndexBuffer(rings.IndexBuffer(),rhi::IndexFormat::UInt32);
    commands.DrawIndexed(rings.IndexCount());
}
} // namespace orbit::celestial_rings
