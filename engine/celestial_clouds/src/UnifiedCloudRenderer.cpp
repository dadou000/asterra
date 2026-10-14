#include <orbit/celestial_clouds/UnifiedCloudRenderer.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace orbit::celestial_clouds::unified
{
namespace
{
constexpr const char* kVertex = R"(
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
V main(uint id : SV_VertexID)
{
    const float2 p[6] = {
        float2(-1,-1),float2(-1,1),float2(1,-1),
        float2(1,-1),float2(-1,1),float2(1,1)
    };
    V result;
    result.position = float4(p[id], 0, 1);
    result.uv = float2(0.5 * (p[id].x + 1), 0.5 * (1 - p[id].y));
    return result;
}
)";

// Independent B shader; not a copy of A's ray marcher, Sun cache, history,
// density profile or shader defines. The sole shared *data* is the packed,
// read-only cubemap weather product from CloudField.
constexpr const char* kMedium = R"(
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
struct Config
{
    float4 cameraAspect; // body-fixed eye xyz, aspect
    float4 forwardFov;   // view forward, tan half vertical fov
    float4 upNear;       // view up, near plane
    float4 sunFar;       // Sun direction, far plane
    float4 shell;        // planet radius, cloud base and top, cubemap face size
    float4 material;     // total layer count, reserved, reserved, irradiance
};
[[vk::push_constant]] Config g;
[[vk::binding(0,0)]] ByteAddressBuffer g_weather;
// Authoritative GPU simulation export, optional. Only the input buffer is
// shared with A; B uses its own storm sampling and optical integration.
[[vk::binding(1,0)]] StructuredBuffer<float> g_storm;
[[vk::binding(2,0)]] [[vk::combinedImageSampler]] Texture2D g_depth;
[[vk::binding(2,0)]] [[vk::combinedImageSampler]] SamplerState g_depthSampler;

float3 CameraRay(float2 uv)
{
    float3 forward = normalize(g.forwardFov.xyz);
    float3 right = normalize(cross(forward, g.upNear.xyz));
    float3 up = normalize(cross(right, forward));
    float2 screen = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    return normalize(forward +
        g.forwardFov.w * (screen.x * g.cameraAspect.w * right + screen.y * up));
}

bool SphereInterval(float3 p, float3 dir, float radius, out float a, out float b)
{
    const float along = dot(p, dir);
    const float3 perp = p - along * dir;
    const float discriminant = radius * radius - dot(perp, perp);
    a = 0; b = 0;
    if (discriminant < 0) return false;
    float h = sqrt(discriminant);
    a = -along - h;
    b = -along + h;
    return b > 0;
}

float SceneStop(float2 uv, float3 ray)
{
    const float d = g_depth.SampleLevel(g_depthSampler, uv, 0).r;
    if (d <= 0) return g.sunFar.w;
    const float nearZ = max(g.upNear.w, 1.e-4);
    const float farZ = max(g.sunFar.w, nearZ + 1.);
    float z = nearZ * farZ / max(d * (farZ - nearZ) + nearZ, 1.e-5);
    return min(g.sunFar.w, z / max(dot(ray, normalize(g.forwardFov.xyz)), 1.e-4));
}

void CubeCoords(float3 n, out uint face, out float2 xy)
{
    const float3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z)
    {
        face = n.x >= 0 ? 0u : 1u;
        xy = n.x >= 0 ? float2(-n.z,n.y) / a.x : float2(n.z,n.y) / a.x;
    }
    else if (a.y >= a.z)
    {
        face = n.y >= 0 ? 2u : 3u;
        xy = n.y >= 0 ? float2(n.x,-n.z) / a.y : float2(n.x,n.z) / a.y;
    }
    else
    {
        face = n.z >= 0 ? 4u : 5u;
        xy = n.z >= 0 ? float2(n.x,n.y) / a.z : float2(-n.x,n.y) / a.z;
    }
}
// One lookup of the original 16-byte weather texels, offset by layer.
// Each layer has its own authored altitude, extinction and optical properties.
float4 WeatherLayer(uint layer, uint face, float2 coord, uint size)
{
    const uint2 ij = (uint2)floor(coord);
    const uint2 ij1 = min(ij + 1u, size - 1u);
    const float2 weight = frac(coord);
    const uint baseIndex = (layer * 6u + face) * size * size;
    const float4 a = asfloat(g_weather.Load4((baseIndex + ij.y * size + ij.x) * 16u));
    const float4 b = asfloat(g_weather.Load4((baseIndex + ij.y * size + ij1.x) * 16u));
    const float4 c = asfloat(g_weather.Load4((baseIndex + ij1.y * size + ij.x) * 16u));
    const float4 d = asfloat(g_weather.Load4((baseIndex + ij1.y * size + ij1.x) * 16u));
    return lerp(lerp(a,b,weight.x), lerp(c,d,weight.x), weight.y);
}
void LayerDescriptor(uint layer, out float4 shell, out float4 optical)
{
    const uint count = (uint)g.material.x;
    const uint size = (uint)g.shell.w;
    const uint offset = count * 6u * size * size * 16u + layer * 32u;
    shell = asfloat(g_weather.Load4(offset));
    optical = asfloat(g_weather.Load4(offset + 16u));
}

// Authoritative storm export layout, see GpuStormSolver::ExportCloud.
// B samples this physical field independently; no A storm shader is reused.
float ParentCell(int3 cell)
{
    const int3 dims = max(int3(g_storm[17],g_storm[18],g_storm[19]),int3(1,1,1));
    const int3 c = clamp(cell, int3(0,0,0), dims - 1);
    return max(0., g_storm[32 + (c.z*dims.y+c.y)*dims.x+c.x]);
}
float ChildCell(int3 cell)
{
    const int3 dims = max(int3(g_storm[25],g_storm[26],g_storm[27]),int3(1,1,1));
    const int3 c = clamp(cell, int3(0,0,0), dims - 1);
    return max(0.,g_storm[(int)g_storm[30] + (c.z*dims.y+c.y)*dims.x+c.x]);
}
float SampleParent(float3 fractional)
{
    int3 a = (int3)floor(fractional);
    float3 t = frac(fractional);
    t = t*t*(3.-2.*t);
    const float x0 = lerp(ParentCell(a),ParentCell(a+int3(1,0,0)),t.x);
    const float x1 = lerp(ParentCell(a+int3(0,1,0)),ParentCell(a+int3(1,1,0)),t.x);
    const float y0 = lerp(ParentCell(a+int3(0,0,1)),ParentCell(a+int3(1,0,1)),t.x);
    const float y1 = lerp(ParentCell(a+int3(0,1,1)),ParentCell(a+int3(1,1,1)),t.x);
    return lerp(lerp(x0,x1,t.y),lerp(y0,y1,t.y),t.z);
}
float SampleChild(float3 fractional)
{
    int3 a = (int3)floor(fractional);
    float3 t = frac(fractional);
    t = t*t*(3.-2.*t);
    const float x0 = lerp(ChildCell(a),ChildCell(a+int3(1,0,0)),t.x);
    const float x1 = lerp(ChildCell(a+int3(0,1,0)),ChildCell(a+int3(1,1,0)),t.x);
    const float y0 = lerp(ChildCell(a+int3(0,0,1)),ChildCell(a+int3(1,0,1)),t.x);
    const float y1 = lerp(ChildCell(a+int3(0,1,1)),ChildCell(a+int3(1,1,1)),t.x);
    return lerp(lerp(x0,x1,t.y),lerp(y0,y1,t.y),t.z);
}
float Influence(float x, float y)
{
    const int offset = (int)g_storm[10];
    if (offset <= 0) return 1.;
    const int2 dims = max(int2(g_storm[17],g_storm[18]),int2(1,1));
    const float2 coord = float2(
        x/max(g_storm[14],1.)+0.5*float(dims.x)-0.5,
        y/max(g_storm[15],1.)+0.5*float(dims.y)-0.5);
    const int2 a = clamp((int2)floor(coord),int2(0,0),dims-1);
    const int2 b = clamp(a+1,int2(0,0),dims-1);
    const float2 t = saturate(coord-floor(coord));
    const float u = lerp(g_storm[offset+a.y*dims.x+a.x],
                         g_storm[offset+a.y*dims.x+b.x], t.x);
    const float v = lerp(g_storm[offset+b.y*dims.x+a.x],
                         g_storm[offset+b.y*dims.x+b.x], t.x);
    return saturate(lerp(u,v,t.y));
}
// Returns world-space inverse-metre storm extinction and spatial blend weight.
// The parent/child sponge and influence mask must be smooth at boundaries.
float2 StormMedium(float3 position, float radius)
{
    if (g_storm[0] < 0.5) return float2(0.,0.);
    const float3 direction = position / max(radius,1.);
    const float3 centre = normalize(float3(g_storm[1],g_storm[2],g_storm[3]));
    if (dot(direction,centre) <= 0.) return float2(0.,0.);
    const float planetR = g_storm[13];
    const float x = planetR*dot(direction,float3(g_storm[4],g_storm[5],g_storm[6]));
    const float y = planetR*dot(direction,float3(g_storm[7],g_storm[8],g_storm[9]));
    const float halfX = 0.5*g_storm[17]*g_storm[14];
    const float halfY = 0.5*g_storm[18]*g_storm[15];
    const float edge = min(halfX-abs(x),halfY-abs(y));
    if (edge <= 0.) return float2(0.,0.);
    const float blend = smoothstep(0.,max(g_storm[20],1.),edge)*
        saturate(g_storm[21])*Influence(x,y);
    if (blend <= 0.) return float2(0.,0.);
    const float altitude = radius - planetR;
    const float dz = max(g_storm[16],1.);
    const float z = altitude/dz-0.5;
    if (z < -1. || z > g_storm[19]) return float2(0.,blend);
    const float3 parentCoord = float3(
        x/max(g_storm[14],1.)+0.5*g_storm[17]-0.5,
        y/max(g_storm[15],1.)+0.5*g_storm[18]-0.5,z);
    float sigma = SampleParent(parentCoord);
    if (g_storm[22] > 0.5)
    {
        const float childX = x - g_storm[28];
        const float childY = y - g_storm[29];
        const float childEdge = min(
            0.5*g_storm[25]*g_storm[23]-abs(childX),
            0.5*g_storm[26]*g_storm[24]-abs(childY));
        if (childEdge > 0.)
        {
            const float childBlend =
                smoothstep(0.,max(g_storm[31],1.),childEdge);
            const float3 childCoord = float3(
                childX/max(g_storm[23],1.)+0.5*g_storm[25]-0.5,
                childY/max(g_storm[24],1.)+0.5*g_storm[26]-0.5,z);
            sigma = lerp(sigma,SampleChild(childCoord),childBlend);
        }
    }
    return float2(max(sigma,0.),blend);
}

// The sum of all participating materials. Convective procedural extinction
// is replaced under a live storm sponge, while independent cirrus and fog
// remain. Returns (total sigma_t, sigma_s, sum(sigma_s*g), unused).
float4 Density(float3 position, bool receiver)
{
    const float radius = length(position);
    if (radius <= g.shell.x) return 0.;
    const float altitude = radius - g.shell.x;
    const float3 direction = position / radius;
    uint face;
    float2 cube;
    CubeCoords(direction,face,cube);
    const uint size = max(2u,(uint)g.shell.w);
    const float2 coord = saturate(cube*0.5+0.5)*float(size-1u);
    const uint layers = (uint)g.material.x;
    float staticOther = 0., staticConv = 0.;
    float otherScatter = 0., convScatter = 0.;
    float otherPhase = 0., convPhase = 0.;
    [loop]
    for (uint layer=0u; layer<layers; ++layer)
    {
        float4 shell, optics;
        LayerDescriptor(layer,shell,optics);
        if (altitude <= shell.x || altitude >= shell.y) continue;
        if (receiver && optics.w < 0.5) continue;
        const float4 w = WeatherLayer(layer,face,coord,size);
        float coverage = saturate((w.x-(0.52-shell.w*0.35-0.26))/0.52);
        if (coverage <= 0.015 && w.y <= 0.015) continue;
        const float h = saturate((altitude-shell.x)/max(shell.y-shell.x,1.));
        const float type = saturate(w.w);
        const float low = smoothstep(0.,0.07,h) * (1.-smoothstep(0.7,1.,h));
        const float fair = smoothstep(0.,0.12,h) *
            (1.-smoothstep(0.62+0.18*type,1.,h));
        const float tower = smoothstep(0.,0.14,h) *
            (1.-smoothstep(0.90,1.,h));
        const float lowToCumulus = smoothstep(0.20,0.50,type);
        const float cumulusToTower = smoothstep(0.58,0.92,type);
        float profile = lerp(lerp(low,fair,lowToCumulus),tower,cumulusToTower);
        const float topSheet = smoothstep(0.65,0.83,h) *
            (1.-smoothstep(0.95,1.,h)) * saturate(w.y)*0.5;
        profile += topSheet;
        const float shape = pow(smoothstep(0.,1.,coverage),max(optics.x,0.1));
        const float detail = 0.75 + 0.25 *
            sin(position.x*0.0013 + 1.7*sin(position.y*0.00083) +
                position.z*0.0011 + float(layer)*1.72);
        const float extinction = max(0.,shell.z) /
            max(shell.y-shell.x,1.) * 2.5 *
            (shape*profile+topSheet*0.18)*detail;
        // Convective clouds in this *layer* are displaced by the live
        // simulated field; unrelated thin decks remain additive.
        const float conv = smoothstep(0.54,0.82,type);
        const float a = saturate(optics.y);
        const float g = clamp(optics.z,-0.98,0.98);
        staticOther += extinction*(1.-conv);
        staticConv += extinction*conv;
        otherScatter += extinction*(1.-conv)*a;
        convScatter += extinction*conv*a;
        otherPhase += extinction*(1.-conv)*a*g;
        convPhase += extinction*conv*a*g;
    }
    const float2 sim = StormMedium(position,radius);
    const float storm = sim.x;
    const float weight = sim.y;
    const float sigma = staticOther+lerp(staticConv,storm,weight);
    const float scattered = otherScatter +
        lerp(convScatter,0.995*storm,weight);
    const float phaseWeight = otherPhase +
        lerp(convPhase,0.72*0.995*storm,weight);
    return float4(max(sigma,0.),max(scattered,0.),phaseWeight,0.);
}

float4 March(V input) : SV_Target0
{
    float3 direction = CameraRay(input.uv);
    float first, last;
    const float outer = g.shell.x + g.shell.z;
    if (!SphereInterval(g.cameraAspect.xyz, direction, outer, first, last))
        return float4(0,0,0,1);
    float groundA, groundB;
    if (SphereInterval(g.cameraAspect.xyz, direction, g.shell.x + g.shell.y, groundA, groundB))
    {
        if (groundA > 0.) last = min(last, groundA);
        else if (groundB > 0.) first = max(first,groundB);
    }
    first = max(first, 0.);
    last = min(last, SceneStop(input.uv, direction));
    if (last <= first)
        return float4(0,0,0,1);

    // B1 intentionally bounded, independent shading; B3 replaces the local
    // sun approximation with a versioned long-range optical-depth cache.
    const uint steps = 64u;
    const float dt = (last - first) / float(steps);
    float T = 1.;
    float3 L = 0;
    const float3 sun = normalize(g.sunFar.xyz);
    [loop]
    for (uint i=0u; i<steps && T>0.004; ++i)
    {
        float3 p = g.cameraAspect.xyz +
            direction * (first + (float(i)+0.5)*dt);
        float4 sample = Density(p,false);
        float sigma = sample.x;
        if (sigma < 1.e-8) continue;
        float upSun = saturate(dot(normalize(p), sun)*0.5+0.5);
        float angle = dot(direction, sun);
        float meanG = clamp(sample.z/max(sample.y,1.e-7),-0.95,0.95);
        float d = max(1.+meanG*meanG-2.*meanG*angle,0.02);
        float phase = (1.-meanG*meanG)/(d*sqrt(d));
        float sunlight = max(0., g.material.w) * upSun * phase *
            saturate(sample.y/max(sigma,1.e-8));
        float ambient = 0.13 + 0.20 * upSun;
        float alpha = 1. - exp(-sigma * dt);
        float3 source = float3(1.0,0.96,0.91) * sunlight +
            float3(0.42,0.57,0.83) * ambient;
        L += T * alpha * source;
        T *= 1. - alpha;
    }
    return float4(L, saturate(T));
}

// Receiver visibility is a different shader / pass and may be tested
// independently of the B main march. Its density function is identical.
float4 ReceiverShadow(V input) : SV_Target0
{
    float3 viewDir = CameraRay(input.uv);
    float sceneDist = SceneStop(input.uv, viewDir);
    if (sceneDist >= g.sunFar.w) return float4(1,1,1,1);
    float3 surface = g.cameraAspect.xyz + viewDir * sceneDist;
    float3 sun = normalize(g.sunFar.xyz);
    float enterT, exitT;
    if (!SphereInterval(surface, sun, g.shell.x + g.shell.z, enterT, exitT))
        return float4(1,1,1,1);
    float segmentStart = max(0., enterT);
    float segmentEnd = max(segmentStart, exitT);
    // The far-side cloud shell must never cast a shadow through the planet.
    float planetNear, planetFar;
    if (SphereInterval(surface, sun, g.shell.x, planetNear, planetFar))
    {
        if (planetNear > 1.)
            segmentEnd = min(segmentEnd, planetNear);
        else if (planetFar > 1. && planetNear <= 0.)
            return float4(1,1,1,1);
    }
    if (segmentEnd <= segmentStart) return float4(1,1,1,1);
    const float delta = (segmentEnd - segmentStart) / 12.;
    float tau = 0.;
    [loop]
    for (uint s=0u; s<12u; ++s)
    {
        float3 p = surface + sun * (segmentStart+(float(s)+0.5)*delta);
        tau += Density(p,true).x * delta;
    }
    float visibility = exp(-tau);
    return float4(visibility,visibility,visibility,1);
}
)";
constexpr const char* kComposite = R"(
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
[[vk::binding(0,0)]] [[vk::combinedImageSampler]] Texture2D g_clouds;
[[vk::binding(0,0)]] [[vk::combinedImageSampler]] SamplerState g_sampler;
float4 main(V input) : SV_Target0
{
    float4 medium = g_clouds.SampleLevel(g_sampler,input.uv,0);
    float opacity = saturate(1.-medium.a);
    if (opacity < 1.e-5) return float4(0,0,0,0);
    return float4(max(medium.rgb,0.)/max(opacity,1.e-5),opacity);
}
)";

[[nodiscard]] u32 Bits(const f32 v) { return std::bit_cast<u32>(v); }
} // namespace

UnifiedCloudRenderer::UnifiedCloudRenderer(rhi::Device& device, const shader::Compiler& compiler)
{
    const auto vertex = compiler.Compile({
        .source = kVertex, .entryPoint = "main", .stage = shader::Stage::Vertex, .debug = false});
    const auto pixel = compiler.Compile({
        .source = kMedium, .entryPoint = "March", .stage = shader::Stage::Pixel, .debug = false});
    const auto shadowPixel = compiler.Compile({
        .source = kMedium, .entryPoint = "ReceiverShadow", .stage = shader::Stage::Pixel, .debug = false});
    const auto compPixel = compiler.Compile({
        .source = kComposite, .entryPoint = "main", .stage = shader::Stage::Pixel, .debug = false});
    const auto pipeline = [&device,&vertex](const auto& fragment,
        const u32 constants, const u32 buffers, const u32 textures, const rhi::BlendMode blend)
    {
        return device.CreateGraphicsPipeline({
            .vertexShader = {.data = vertex.bytecode.data(), .size = vertex.bytecode.size()},
            .pixelShader = {.data = fragment.bytecode.data(), .size = fragment.bytecode.size()},
            .vertexAttributes = {}, .vertexStrideBytes = 0U,
            .pushConstantDwords = constants, .shaderResourceBuffers = buffers,
            .sampledTextures = textures, .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid, .cullMode = rhi::CullMode::None,
            .blendMode = blend, .depthTest = false, .depthWrite = false,
            .colorAttachmentFormats = {rhi::TextureFormat::RGBA16_Float},
            .colorAttachmentCount = 1U
        });
    };
    march_ = pipeline(pixel, 24U, 2U, 1U, rhi::BlendMode::Opaque);
    receiverShadow_ = pipeline(shadowPixel, 24U, 2U, 1U, rhi::BlendMode::Opaque);
    composite_ = pipeline(compPixel, 0U, 0U, 1U, rhi::BlendMode::Alpha);
    if (!march_ || !receiverShadow_ || !composite_)
        throw std::runtime_error("Unified System B cloud pipelines unavailable");
    emptyStorm_ = device.CreateBuffer({
        .sizeBytes = 64ULL * sizeof(f32),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    if (!emptyStorm_)
        throw std::runtime_error("Unified System B inactive storm allocation failed");
    std::memset(emptyStorm_->Map(), 0, 64U * sizeof(f32));
    emptyStorm_->Unmap();
}
UnifiedCloudRenderer::~UnifiedCloudRenderer() = default;

void UnifiedCloudRenderer::DrawMedium(
    rhi::CommandList& commands, const bool shadow,
    rhi::Texture& depth, GpuCloudFieldProduct& field, rhi::Texture& target,
    const f64 bodyRadiusMeters, const CloudFieldProduct& source,
    const celestial_atmosphere::AtmosphereRenderView& view,
    rhi::Buffer* const stormExport)
{
    if (!target.Width() || !target.Height() || field.FaceResolution() < 2U ||
        source.layers.empty() || field.LayerCount() != source.layers.size() ||
        field.FaceResolution() != source.faceResolution)
        return;
    // Every B density layer uses its own altitude interval. The outer
    // intersection and inner bound must encompass all of them, including
    // cirrus and optically thin foreground fog.
    f64 base = std::numeric_limits<f64>::max();
    f64 top = 0.0;
    for (const auto& layer : source.layers)
    {
        base = std::min(base, layer.parameters.baseAltitudeMeters);
        top = std::max(top, layer.parameters.topAltitudeMeters);
    }
    if (!(top > base)) return;
    const std::array<u32, 24> constants{
        Bits(static_cast<f32>(view.cameraPositionMeters.x)),
        Bits(static_cast<f32>(view.cameraPositionMeters.y)),
        Bits(static_cast<f32>(view.cameraPositionMeters.z)),
        Bits(static_cast<f32>(target.Width())/static_cast<f32>(target.Height())),
        Bits(view.forward.x), Bits(view.forward.y), Bits(view.forward.z),
        Bits(std::tan(view.verticalFovRadians * 0.5F)),
        Bits(view.up.x), Bits(view.up.y), Bits(view.up.z),
        Bits(view.nearPlaneMeters),
        Bits(view.sunDirection.x), Bits(view.sunDirection.y), Bits(view.sunDirection.z),
        Bits(view.farPlaneMeters),
        Bits(static_cast<f32>(bodyRadiusMeters)),
        Bits(static_cast<f32>(base)),
        Bits(static_cast<f32>(top)),
        Bits(static_cast<f32>(field.FaceResolution())),
        Bits(static_cast<f32>(field.LayerCount())),
        0U,
        0U,
        Bits(view.irradianceScale)
    };
    commands.SetRenderTarget(target);
    commands.SetViewport({.x = 0.0F, .y = 0.0F,
        .width = static_cast<f32>(target.Width()),
        .height = static_cast<f32>(target.Height()),
        .minDepth = 0.0F, .maxDepth = 1.0F});
    commands.SetScissor({.left = 0, .top = 0,
        .right = static_cast<i32>(target.Width()),
        .bottom = static_cast<i32>(target.Height())});
    commands.SetGraphicsPipeline(shadow ? *receiverShadow_ : *march_);
    commands.SetGraphicsConstants(constants);
    commands.SetGraphicsBuffer(0, field.Buffer());
    commands.SetGraphicsBuffer(1,
        stormExport != nullptr ? *stormExport : *emptyStorm_);
    commands.SetGraphicsTexture(0, depth);
    commands.Draw(6);
}

void UnifiedCloudRenderer::Draw(
    rhi::CommandList& commands, rhi::Texture& depth,
    GpuCloudFieldProduct& field, rhi::Texture& target,
    const f64 radius, const CloudFieldProduct& source,
    const celestial_atmosphere::AtmosphereRenderView& view,
    rhi::Buffer* const stormExport)
{
    DrawMedium(commands, false, depth, field, target, radius, source, view,
        stormExport);
}
void UnifiedCloudRenderer::DrawReceiverShadow(
    rhi::CommandList& commands, rhi::Texture& depth,
    GpuCloudFieldProduct& field, rhi::Texture& target,
    const f64 radius, const CloudFieldProduct& source,
    const celestial_atmosphere::AtmosphereRenderView& view,
    rhi::Buffer* const stormExport)
{
    DrawMedium(commands, true, depth, field, target, radius, source, view,
        stormExport);
}
void UnifiedCloudRenderer::Composite(
    rhi::CommandList& commands, rhi::Texture& medium, rhi::Texture& scene,
    const u32 width, const u32 height)
{
    if (!width || !height) return;
    commands.SetRenderTarget(scene);
    commands.SetViewport({.x = 0.0F, .y = 0.0F,
        .width = static_cast<f32>(width), .height = static_cast<f32>(height),
        .minDepth = 0.0F, .maxDepth = 1.0F});
    commands.SetScissor({.left = 0, .top = 0,
        .right = static_cast<i32>(width), .bottom = static_cast<i32>(height)});
    commands.SetGraphicsPipeline(*composite_);
    commands.SetGraphicsTexture(0, medium);
    commands.Draw(6);
}
} // namespace orbit::celestial_clouds::unified
