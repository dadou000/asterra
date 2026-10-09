#include <orbit/lighting/HybridReflectionRenderer.hpp>
#include <orbit/lighting/SdfTraceShader.hpp>
#include <orbit/lighting/ReflectionTraceShader.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
constexpr const char* kCs = R"(
struct GpuRadianceCell
{
    float4 irradiance0;
    float4 irradianceX;
    float4 irradianceY;
    float4 irradianceZ;
    float4 skyIrradiance; // rgb sky-only L0
    float4 skyGradient;   // xyz sky-only gradient, w one-bounce transport
};

struct GpuRadianceLevelInfo
{
    float4 centerCellSize;
    uint4 moduloAxis;
    uint4 offsetCountLevel;
};

[[vk::binding(0, 0)]]
StructuredBuffer<GpuRadianceCell> g_cells : register(t0);

[[vk::binding(1, 0)]]
StructuredBuffer<GpuRadianceLevelInfo> g_levels : register(t1);

// Merged mesh distance field: what a reflected ray meets once it leaves the
// screen (dummies when absent). Corner-packed distances, see SdfTraceShader.hpp.
#define SDF_DIST_CORNERS 1
[[vk::binding(2, 0)]] RWStructuredBuffer<uint4> g_sdfDist : register(u20);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> g_sdfAlbedo : register(u21);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> g_sdfNormal : register(u22);
[[vk::binding(5, 0)]] RWStructuredBuffer<float4> g_sdfRadiance : register(u23);

[[vk::binding(9, 0)]]
RWTexture2D<float4> g_target : register(u2);

[[vk::binding(10, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_sceneColor : register(t3);
[[vk::binding(10, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sceneSampler : register(s3);

[[vk::binding(11, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_baseRoughness : register(t4);
[[vk::binding(11, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_baseSampler : register(s4);

[[vk::binding(12, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_normalMetallic : register(t5);
[[vk::binding(12, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_normalSampler : register(s5);

[[vk::binding(13, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth : register(t6);
[[vk::binding(13, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler : register(s6);

[[vk::binding(14, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_emissionClass : register(t7);
[[vk::binding(14, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_emissionSampler : register(s7);

struct Constants
{
    uint width;
    uint height;
    uint levelCount;
    uint maximumSteps;

    float4 forwardAspect;
    float4 upTanHalfFov;
    float4 depthTrace;
    float4 reflectionTuning;
    float4 sdfOriginVoxel;       // volume voxel (0,0,0) centre relative to the camera, voxel size
    float4 exactOriginCount;
    float4 sdfDimensionsEnable;  // dimensions, enable (1 = a field is bound)
};

[[vk::push_constant]]
Constants g;

#define SDF_ORIGIN g.sdfOriginVoxel.xyz
#define SDF_VOXEL g.sdfOriginVoxel.w
#define SDF_DIMS int3(g.sdfDimensionsEnable.xyz)
//SDF_TRACE_INCLUDE
//REFLECTION_TRACE_INCLUDE

// How far a reflected ray looks for the room (metres).
static const float kSdfReflectionDistance = 40.0;

float ReverseZViewDepth(float depth)
{
    const float nearPlane =
        max(g.depthTrace.x, 1.0e-5);
    const float farPlane =
        max(g.depthTrace.y, nearPlane + 1.0e-4);

    return
        nearPlane * farPlane /
        max(
            depth * (farPlane - nearPlane) +
                nearPlane,
            1.0e-6);
}

void CameraBasis(
    out float3 forward,
    out float3 right,
    out float3 up,
    out float aspect,
    out float tanHalfFov)
{
    forward = normalize(g.forwardAspect.xyz);
    const float3 requestedUp =
        normalize(g.upTanHalfFov.xyz);
    right =
        normalize(cross(forward, requestedUp));
    up =
        normalize(cross(right, forward));
    aspect =
        max(g.forwardAspect.w, 0.001);
    tanHalfFov =
        max(g.upTanHalfFov.w, 0.001);
}

float3 ViewRay(float2 uv)
{
    float3 forward;
    float3 right;
    float3 up;
    float aspect;
    float tanHalfFov;
    CameraBasis(
        forward,
        right,
        up,
        aspect,
        tanHalfFov);

    const float2 ndc = {
        uv.x * 2.0 - 1.0,
        1.0 - uv.y * 2.0
    };

    return normalize(
        forward +
        right *
            (ndc.x * aspect * tanHalfFov) +
        up *
            (ndc.y * tanHalfFov));
}

float3 ReconstructPosition(
    float2 uv,
    float depth)
{
    const float3 ray =
        ViewRay(uv);
    const float3 forward =
        normalize(g.forwardAspect.xyz);
    const float viewDepth =
        ReverseZViewDepth(depth);

    return
        ray *
        (viewDepth /
         max(dot(ray, forward), 1.0e-5));
}

bool ProjectPoint(
    float3 position,
    out float2 uv,
    out float viewDepth)
{
    float3 forward;
    float3 right;
    float3 up;
    float aspect;
    float tanHalfFov;

    CameraBasis(
        forward,
        right,
        up,
        aspect,
        tanHalfFov);

    viewDepth =
        dot(position, forward);

    if (viewDepth <=
        max(g.depthTrace.x, 1.0e-5))
    {
        uv = 0.0;
        return false;
    }

    const float2 ndc = {
        dot(position, right) /
            (viewDepth * tanHalfFov * aspect),
        dot(position, up) /
            (viewDepth * tanHalfFov)
    };

    uv = float2(
        ndc.x * 0.5 + 0.5,
        0.5 - ndc.y * 0.5
    );

    return
        all(uv >= 0.0) &&
        all(uv <= 1.0);
}

int PositiveModulo(
    int value,
    int modulus)
{
    const int result =
        value % modulus;

    return
        result < 0
            ? result + modulus
            : result;
}

float3 SampleCache(
    float3 position,
    float3 direction)
{
    [loop]
    for (uint levelIndex = 0u;
         levelIndex < g.levelCount;
         ++levelIndex)
    {
        const GpuRadianceLevelInfo level =
            g_levels[levelIndex];

        const float cellSize =
            max(level.centerCellSize.w, 1.0e-5);
        const int axis =
            int(level.moduloAxis.w);

        if (axis <= 0)
        {
            continue;
        }

        const int3 delta =
            int3(
                round(
                    (position -
                     level.centerCellSize.xyz) /
                    cellSize));

        const int halfAxis =
            axis / 2;

        // The level's window is [-halfAxis, halfAxis - 1] cells around its centre (ScrollTo keeps
        // cellsPerAxis cells). +halfAxis is outside it and aliases to the -halfAxis slot on the far
        // side of the cube, which painted a one-cell-wide wrong-valued plane at the window edge.
        if (any(delta < -halfAxis) || any(delta >= halfAxis))
        {
            continue;
        }

        const int px =
            PositiveModulo(
                int(level.moduloAxis.x) +
                    delta.x,
                axis);
        const int py =
            PositiveModulo(
                int(level.moduloAxis.y) +
                    delta.y,
                axis);
        const int pz =
            PositiveModulo(
                int(level.moduloAxis.z) +
                    delta.z,
                axis);

        const uint localIndex =
            uint(px) +
            uint(axis) *
                (uint(py) +
                 uint(axis) * uint(pz));

        if (localIndex >=
            level.offsetCountLevel.y)
        {
            continue;
        }

        const GpuRadianceCell cell =
            g_cells[
                level.offsetCountLevel.x +
                localIndex];

        if (cell.irradiance0.w <= 0.5)
        {
            continue;
        }

        const float3 d =
            normalize(direction);

        // The sky now lives in its own channel; reflections keep the
        // one-bounce sky the L1 used to carry: 2 * l0 * transport is the
        // scaled sky irradiance, with the old L0 / L1 lobe weights.
        const float3 legacySky =
            cell.skyIrradiance.rgb *
            (2.0 * cell.skyGradient.w) *
            (1.175 + 0.175 * dot(cell.skyGradient.xyz, d));

        return max(
            cell.irradiance0.rgb +
            cell.irradianceX.rgb * d.x +
            cell.irradianceY.rgb * d.y +
            cell.irradianceZ.rgb * d.z +
            legacySky,
            0.0);
    }

    return 0.0;
}

float3 FresnelSchlick(
    float cosTheta,
    float3 f0)
{
    return
        f0 +
        (1.0 - f0) *
        pow(
            1.0 - saturate(cosTheta),
            5.0);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= g.width ||
        dispatchId.y >= g.height)
    {
        return;
    }

    const uint2 pixel =
        dispatchId.xy;
    const float2 uv =
        (float2(pixel) + 0.5) /
        float2(g.width, g.height);

    const float4 scene =
        g_sceneColor.SampleLevel(
            g_sceneSampler,
            uv,
            0);

    const float depth =
        g_depth.SampleLevel(
            g_depthSampler,
            uv,
            0).r;

    if (depth <= 0.0)
    {
        g_target[pixel] = float4(0.0, 0.0, 0.0, -1.0);
        return;
    }

    const float4 baseRoughness =
        g_baseRoughness.SampleLevel(
            g_baseSampler,
            uv,
            0);

    const float4 normalMetallic =
        g_normalMetallic.SampleLevel(
            g_normalSampler,
            uv,
            0);

    const float roughness =
        saturate(baseRoughness.a);
    const float metallic =
        saturate(normalMetallic.a);
    const float3 normal =
        normalize(normalMetallic.xyz);

    const float3 viewRay =
        ViewRay(uv);
    const float3 viewToCamera =
        -viewRay;

    const float3 reflectionDirection =
        normalize(
            reflect(
                viewRay,
                normal));

    const float3 position =
        ReconstructPosition(
            uv,
            depth);

    // Native smooth-surface work only. Rough GI stays in its existing gather.
    if (roughness > g_reflectionLighting[4].x)
    {
        g_target[pixel] = float4(0.0, 0.0, 0.0, -1.0);
        return;
    }
    float hitDistance = 0.0;
    if (g.exactOriginCount.w > 0.0)
    {
        const ReflectionHit exactHit = ReflectionTrace(
            position + normal * 0.003, reflectionDirection, 0.001, g.depthTrace.z);
        if (exactHit.status == 1u)
        {
            float3 closer; float closerT;
            const bool fieldCloser = g.sdfDimensionsEnable.w > 0.5 &&
                SdfTrace(position + normal * (0.6*SDF_VOXEL), reflectionDirection,
                    max(exactHit.t - 0.8*SDF_VOXEL, 0.001), closer, closerT);
            if (fieldCloser)
            {
                g_target[pixel] = float4(SdfFetchRadiance(closer, -reflectionDirection), closerT);
                return;
            }
            if (!fieldCloser)
            {
                const float3 hitRadiance = ReflectionShadeHit(
                    exactHit, -reflectionDirection, g.sdfDimensionsEnable.w > 0.5);
                // Fresnel is applied only in the composite; history stores radiance.
                g_target[pixel] = float4(hitRadiance, exactHit.t);
                return;
            }
        }
    }
    float3 reflectedRadiance = 0.0;
    float screenConfidence = 0.0;

    const float maxScreenRoughness =
        max(g.reflectionTuning.x, 0.0);

    if (roughness <= maxScreenRoughness)
    {
        const float traceRadius =
            max(g.depthTrace.z, 0.1) *
            lerp(
                1.0,
                0.35,
                saturate(
                    roughness /
                    max(
                        maxScreenRoughness,
                        1.0e-4)));

        const float thickness =
            max(g.depthTrace.w, 0.001);

        const uint steps =
            max(g.maximumSteps, 2u);

        float previousDelta =
            -1.0e20;

        [loop]
        for (uint step = 1u;
             step <= steps;
             ++step)
        {
            const float t =
                traceRadius *
                (float(step) /
                 float(steps));

            const float3 queryPosition =
                position +
                normal * 0.03 +
                reflectionDirection * t;

            float2 hitUv;
            float queryViewDepth;

            if (!ProjectPoint(
                    queryPosition,
                    hitUv,
                    queryViewDepth))
            {
                break;
            }

            const float hitDepth =
                g_depth.SampleLevel(
                    g_depthSampler,
                    hitUv,
                    0).r;

            if (hitDepth <= 0.0)
            {
                previousDelta =
                    -1.0e20;
                continue;
            }

            const float sceneViewDepth =
                ReverseZViewDepth(
                    hitDepth);

            const float delta =
                queryViewDepth -
                sceneViewDepth;

            if (delta >= -thickness &&
                previousDelta < -thickness)
            {
                hitDistance = t;
                reflectedRadiance =
                    max(
                        g_sceneColor.SampleLevel(
                            g_sceneSampler,
                            hitUv,
                            roughness * 4.0).rgb,
                        0.0);

                const float edge =
                    min(
                        min(hitUv.x, 1.0 - hitUv.x),
                        min(hitUv.y, 1.0 - hitUv.y));

                screenConfidence =
                    saturate(
                        edge / 0.04);
                break;
            }

            previousDelta = delta;
        }
    }

    // Authored proxy surfaces (class 3) do not take the cache fallback: its
    // cube cells straddle thin walls and leak outside light through them.
    const float surfaceClass =
        floor(
            g_emissionClass.SampleLevel(
                g_emissionSampler,
                uv,
                0).a + 0.01);

    const float3 cacheRadiance =
        surfaceClass == 3.0
            ? float3(0.0, 0.0, 0.0)
            : SampleCache(
                  position,
                  reflectionDirection) *
              max(g.reflectionTuning.z, 0.0);

    // What the screen could not show: for smooth surfaces trace the mesh
    // distance field and read the lit surface it meets (the room behind the
    // camera, the walls beside it); only where that finds nothing does the
    // radiance cache's low-frequency light (mostly sky) stand in.
    float3 offscreenRadiance = cacheRadiance;
    if (g.sdfDimensionsEnable.w > 0.5 &&
        screenConfidence < 0.999 &&
        roughness <= 0.35)
    {
        float3 sdfHit;
        float sdfT;
        const float3 sdfOrigin =
            position + normal * (0.6 * SDF_VOXEL);
        if (SdfTrace(
                sdfOrigin,
                reflectionDirection,
                g.depthTrace.z,
                sdfHit,
                sdfT))
        {
            hitDistance = sdfT;
            offscreenRadiance =
                SdfFetchRadiance(sdfHit, -reflectionDirection);
        }
    }

    reflectedRadiance =
        lerp(
            offscreenRadiance,
            reflectedRadiance,
            screenConfidence);

    // A blended edge/fallback has no single hit distance for reprojection.
    if (screenConfidence > 0.0 && screenConfidence < 0.999) hitDistance = 0.0;

    g_target[pixel] = float4(max(reflectedRadiance, 0.0), hitDistance);

}
)";


constexpr const char* kCompositeCs = R"(
[[vk::binding(0, 0)]] StructuredBuffer<float4> g_params;
[[vk::binding(1, 0)]] RWTexture2D<float4> g_target;
[[vk::binding(2, 0)]] RWTexture2D<float4> g_historyOut;
[[vk::binding(3, 0)]] RWTexture2D<float4> g_metaOut;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] Texture2D<float4> g_raw;
[[vk::binding(4, 0)]] [[vk::combinedImageSampler]] SamplerState s_raw;
[[vk::binding(5, 0)]] [[vk::combinedImageSampler]] Texture2D<float4> g_scene;
[[vk::binding(5, 0)]] [[vk::combinedImageSampler]] SamplerState s_scene;
[[vk::binding(6, 0)]] [[vk::combinedImageSampler]] Texture2D<float4> g_base;
[[vk::binding(6, 0)]] [[vk::combinedImageSampler]] SamplerState s_base;
[[vk::binding(7, 0)]] [[vk::combinedImageSampler]] Texture2D<float4> g_normal;
[[vk::binding(7, 0)]] [[vk::combinedImageSampler]] SamplerState s_normal;
[[vk::binding(8, 0)]] [[vk::combinedImageSampler]] Texture2D<float4> g_depth;
[[vk::binding(8, 0)]] [[vk::combinedImageSampler]] SamplerState s_depth;
[[vk::binding(9, 0)]] [[vk::combinedImageSampler]] Texture2D<float4> g_history;
[[vk::binding(9, 0)]] [[vk::combinedImageSampler]] SamplerState s_history;
[[vk::binding(10, 0)]] [[vk::combinedImageSampler]] Texture2D<float4> g_meta;
[[vk::binding(10, 0)]] [[vk::combinedImageSampler]] SamplerState s_meta;
struct Constants { uint width; uint height; uint historyValid; uint debugView; };
[[vk::push_constant]] Constants g;
float3 Ray(float2 uv, uint camera)
{
    const float4 f = g_params[camera];
    const float4 u = g_params[camera+1u];
    const float3 forward = normalize(f.xyz);
    const float3 right = normalize(cross(forward, u.xyz));
    const float3 up = cross(right, forward);
    return normalize(forward + right * ((uv.x*2.0-1.0)*f.w*u.w) + up * ((1.0-uv.y*2.0)*u.w));
}
float3 VirtualPosition(float2 uv, float cameraDistance, float hitDistance, uint camera)
{
    // Unfold the reflected path behind its local tangent plane. This follows
    // the reflected image, instead of the receiver's ordinary motion vector.
    return Ray(uv,camera) * (cameraDistance + hitDistance);
}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID)
{
    if(id.x>=g.width||id.y>=g.height) return;
    const int2 pixel=int2(id.xy);
    const float2 size=float2(g.width,g.height), uv=(float2(pixel)+0.5)/size;
    const float4 raw=g_raw.Load(int3(pixel,0));
    const float4 scene=g_scene.Load(int3(pixel,0));
    if(raw.w<0.0)
    {
        g_target[pixel]=scene;g_historyOut[pixel]=raw;g_metaOut[pixel]=0.0;return;
    }
    const float4 base=g_base.Load(int3(pixel,0));
    const float4 normal=g_normal.Load(int3(pixel,0));
    const float3 n=normalize(normal.xyz),ray=Ray(uv,0u);
    const float depth=g_depth.Load(int3(pixel,0)).r;
    const float nearPlane=g_params[2].x,farPlane=g_params[2].y;
    const float viewZ=nearPlane*farPlane/max(depth*(farPlane-nearPlane)+nearPlane,1.0e-6);
    const float cameraDistance=viewZ/max(dot(ray,normalize(g_params[0].xyz)),1.0e-5);
    float3 result=max(raw.rgb,0.0);
    float3 lo=result,hi=result;
    // Tight edge-aware neighbourhood bounds for temporal rejection; the
    // native mirror signal itself is never spatially blurred.
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x)
    {
        const int2 q=clamp(pixel+int2(x,y),int2(0,0),int2(g.width-1u,g.height-1u));
        const float4 candidate=g_raw.Load(int3(q,0));
        const float3 qn=normalize(g_normal.Load(int3(q,0)).xyz);
        const float qb=g_base.Load(int3(q,0)).a;
        if(candidate.w<0.0||dot(qn,n)<0.98||abs(qb-base.a)>0.02||
            abs(candidate.w-raw.w)>max(0.05,0.02*raw.w)) continue;
        lo=min(lo,candidate.rgb);hi=max(hi,candidate.rgb);
    }
    if(g.historyValid!=0u && raw.w>0.0 && cameraDistance<65000.0)
    {
        const float3 virtualPrevious=VirtualPosition(uv,cameraDistance,raw.w,0u)+g_params[6].xyz;
        const float3 pf=normalize(g_params[3].xyz);
        const float3 pr=normalize(cross(pf,g_params[4].xyz));
        const float3 pu=cross(pr,pf);
        const float z=dot(virtualPrevious,pf);
        const float2 previousUv=float2(
            0.5+0.5*dot(virtualPrevious,pr)/(max(z,1.0e-5)*g_params[3].w*g_params[4].w),
            0.5-0.5*dot(virtualPrevious,pu)/(max(z,1.0e-5)*g_params[4].w));
        if(z>g_params[5].x && all(previousUv>=0.0)&&all(previousUv<1.0))
        {
            // Point fetch metadata to avoid interpolating across reflection edges.
            const int2 hp=int2(previousUv*size);
            const float2 centre=(float2(hp)+0.5)/size;
            const float4 previous=g_history.Load(int3(hp,0));
            const float4 meta=g_meta.Load(int3(hp,0));
            const float3 oldVirtual=VirtualPosition(centre,meta.w,previous.w,3u);
            const float tolerance=max(0.03,0.002*length(virtualPrevious));
            if(previous.w>0.0&&meta.w>0.0&&dot(n,normalize(meta.xyz))>0.995&&
                length(oldVirtual-virtualPrevious)<tolerance)
            {
                const float3 oldColor=clamp(previous.rgb,lo,hi);
                const float difference=length(oldColor-result)/max(length(result),0.01);
                const float weight=0.65*saturate(1.0-difference);
                result=lerp(result,oldColor,weight);
            }
        }
    }
    const float3 f0=lerp(0.04.xxx,max(base.rgb,0.0),saturate(normal.w));
    const float3 fresnel=f0+(1.0-f0)*pow(1.0-saturate(dot(n,-ray)),5.0);
    const float3 contribution=result*fresnel*(1.0-0.55*base.a*base.a);
    float3 output=scene.rgb+contribution;
    if(g.debugView==1u)output=result;
    if(g.debugView==2u)output=raw.w>0.0?float3(saturate(raw.w/40.0),1.0-saturate(raw.w/40.0),0.0):float3(0.5,0.0,0.5);
    g_target[pixel]=float4(max(output,0.0),scene.a);
    g_historyOut[pixel]=float4(result,raw.w);
    g_metaOut[pixel]=float4(n,cameraDistance);
}
)";

[[nodiscard]] std::string ReflectionSource()
{
    std::string source = kCs;
    const std::string marker = "//SDF_TRACE_INCLUDE";
    source.replace(
        source.find(marker), marker.size(), kSdfTraceHlsl);
    const std::string trace(kReflectionTraceHlsl);
    const auto typesEnd = trace.find("bool ReflectionBox");
    const std::string bindings = R"(
[[vk::binding(6, 0)]] StructuredBuffer<ReflectionTriangle> g_reflectionTriangles;
[[vk::binding(7, 0)]] StructuredBuffer<ReflectionNode> g_reflectionNodes;
[[vk::binding(8, 0)]] StructuredBuffer<float4> g_reflectionLighting;
#define REFLECTION_ORIGIN_COUNT g.exactOriginCount
#ifdef REFLECTION_HARDWARE
[[vk::binding(15, 0)]] RaytracingAccelerationStructure g_reflectionScene;
#endif
)";
    const std::string exactMarker = "//REFLECTION_TRACE_INCLUDE";
    source.replace(source.find(exactMarker), exactMarker.size(),
        trace.substr(0, typesEnd) + bindings + trace.substr(typesEnd));
    return source;
}
} // namespace

std::string BuildHybridReflectionShaderSource(const bool hardware)
{
    return (hardware ? "#define REFLECTION_HARDWARE 1\n" : "") + ReflectionSource();
}
std::string BuildReflectionCompositeShaderSource() { return kCompositeCs; }

HybridReflectionRenderer::
HybridReflectionRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : device_(device)
{
    const auto shader =
        compiler.Compile({
            .source = BuildHybridReflectionShaderSource(),
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    pipeline_ =
        device.CreateComputePipeline({
            .computeShader = {
                .data = shader.bytecode.data(),
                .size = shader.bytecode.size()
            },
            .pushConstantDwords = 32U,
            .shaderResourceBuffers = 9U,
            .storageTextures = 1U,
            .sampledTextures = 5U
        });

    if (device.Capabilities().rayQuery && device.Capabilities().accelerationStructures)
    {
        const auto hardware = compiler.Compile({
            .source = BuildHybridReflectionShaderSource(true),
            .entryPoint = "main", .stage = shader::Stage::Compute,
            .shaderModelMajor = 6U, .shaderModelMinor = 5U,
            .enableSpirvRayQuery = true, .debug = false});
        hardwarePipeline_ = device.CreateComputePipeline({
            .computeShader = {.data = hardware.bytecode.data(), .size = hardware.bytecode.size()},
            .pushConstantDwords = 32U, .shaderResourceBuffers = 9U,
            .storageTextures = 1U, .sampledTextures = 5U, .accelerationStructures = 1U});
    }

    const auto composite = compiler.Compile({.source = BuildReflectionCompositeShaderSource(),
        .entryPoint = "main", .stage = shader::Stage::Compute, .debug = false});
    compositePipeline_ = device.CreateComputePipeline({
        .computeShader = {.data = composite.bytecode.data(), .size = composite.bytecode.size()},
        .pushConstantDwords = 4U, .shaderResourceBuffers = 1U,
        .storageTextures = 3U, .sampledTextures = 7U});

    dummySdf_ = device.CreateBuffer({
        .sizeBytes = 64U,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::UnorderedAccess
    });
    std::memset(
        dummySdf_->Map(),
        0,
        static_cast<std::size_t>(dummySdf_->SizeBytes()));
    dummySdf_->Unmap();
}

void HybridReflectionRenderer::BeginFrame(const u32 completedSlot, const u32 framesInFlight)
{
    if (framesInFlight == 0U || framesInFlight > 64U || completedSlot >= framesInFlight)
        throw std::invalid_argument("Invalid reflection frame slot");
    pendingFramesMask_ = framesInFlight == 64U ? ~0ULL : ((1ULL << framesInFlight) - 1ULL);
    const u64 completed = 1ULL << completedSlot;
    for (auto& p : parameters_) p.pendingFrames &= ~completed;
    std::erase_if(parameters_, [](const auto& p) { return p.pendingFrames == 0U; });
    for (auto& h : retiredHistories_) h.second &= ~completed;
    std::erase_if(retiredHistories_, [](const auto& h) { return h.second == 0U; });
    ++tick_;
}

void HybridReflectionRenderer::Resolve(
    rhi::CommandList& commands,
    rhi::Texture& sceneColor,
    rhi::Texture& surfaceBaseRoughness,
    rhi::Texture& surfaceNormalMetallic,
    rhi::Texture& surfaceEmissionClass,
    rhi::Texture& depth,
    rhi::Buffer& radianceCells,
    rhi::Buffer& radianceLevels,
    const u32 radianceLevelCount,
    rhi::Texture& targetSceneColor,
    const u32 width,
    const u32 height,
    const LightingView& view,
    const f32 qualityScale,
    const HybridReflectionSettings& settings,
    const SdfGatherInput* const sdf,
    const ReflectionSceneInput* const exact)
{
    if (width == 0U ||
        height == 0U ||
        radianceLevelCount == 0U)
    {
        return;
    }

    const auto bits =
        [](const f32 value)
        {
            return std::bit_cast<u32>(value);
        };

    const f32 quality =
        std::clamp(
            qualityScale,
            0.0F,
            1.0F);

    const u32 steps =
        std::clamp(
            static_cast<u32>(
                std::lround(
                    2.0F +
                    quality *
                    static_cast<f32>(
                        std::max(
                            settings.maximumSteps,
                            2U) -
                        2U))),
            2U,
            std::max(
                settings.maximumSteps,
                2U));

    const bool sdfAvailable =
        sdf != nullptr && sdf->distance != nullptr &&
        sdf->albedo != nullptr && sdf->normal != nullptr &&
        sdf->radiance != nullptr;
    const math::Double3 sdfOriginRelative = sdfAvailable
        ? sdf->originInFrameMeters - view.cameraPositionInFrameMeters
        : math::Double3{};

    const bool exactAvailable = settings.exactTriangles && exact != nullptr &&
        exact->triangles != nullptr && exact->nodes != nullptr && exact->nodeCount > 0U;
    const auto exactOrigin = exactAvailable
        ? exact->originInFrameMeters - view.cameraPositionInFrameMeters : math::Double3{};
    const std::array<math::Float4, 5> light{
        math::Float4{exact ? exact->toSun.x : 0.0F, exact ? exact->toSun.y : 1.0F, exact ? exact->toSun.z : 0.0F, 0.0F},
        math::Float4{exact ? exact->sunIrradiance.x : 0.0F, exact ? exact->sunIrradiance.y : 0.0F, exact ? exact->sunIrradiance.z : 0.0F, 0.0F},
        math::Float4{exact ? exact->skyIrradiance.x : 0.0F, exact ? exact->skyIrradiance.y : 0.0F, exact ? exact->skyIrradiance.z : 0.0F, 0.0F},
        math::Float4{exact ? exact->localUp.x : 0.0F, exact ? exact->localUp.y : 1.0F, exact ? exact->localUp.z : 0.0F, 1024.0F},
        math::Float4{std::clamp(settings.maximumSmoothRoughness, 0.0F, 1.0F), 0.0F, 0.0F, 0.0F}};
    auto lightingBuffer = device_.CreateBuffer({.sizeBytes = sizeof(light),
        .usage = rhi::BufferUsage::Structured, .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    std::memcpy(lightingBuffer->Map(), light.data(), sizeof(light));
    lightingBuffer->Unmap();

    auto& history = histories_[&sceneColor];
    if (history.raw == nullptr || history.raw->Width() != width || history.raw->Height() != height)
    {
        if (history.raw != nullptr) retiredHistories_.emplace_back(std::move(history), pendingFramesMask_);
        history = History{};
        const auto texture = [&]() { return device_.CreateTexture({
            .width = width, .height = height, .format = rhi::TextureFormat::RGBA16_Float,
            .initialState = rhi::ResourceState::ShaderResource, .allowUnorderedAccess = true}); };
        history.raw = texture();
        for (u32 i = 0U; i < 2U; ++i) { history.radiance[i] = texture(); history.metadata[i] = texture(); }
    }
    for (auto it = histories_.begin(); it != histories_.end();)
    {
        if (it->first != &sceneColor && tick_ > it->second.lastTick + 96U)
        {
            retiredHistories_.emplace_back(std::move(it->second), pendingFramesMask_);
            it = histories_.erase(it);
        }
        else ++it;
    }
    const u64 geometryRevision = (exactAvailable ? exact->revision : 0U) ^
        (sdfAvailable ? static_cast<u64>(reinterpret_cast<std::uintptr_t>(sdf->distance)) : 0U);
    const bool compatible = settings.temporal && history.valid && history.lastTick + 1U == tick_ &&
        history.geometryRevision == geometryRevision && history.view.frame == view.frame &&
        history.view.body == view.body &&
        std::abs(history.view.verticalFovRadians - view.verticalFovRadians) < 1.0e-5F &&
        history.view.nearPlaneMeters == view.nearPlaneMeters &&
        history.view.farPlaneMeters == view.farPlaneMeters &&
        !HasChange(view.change, LightingViewChange::CameraCut) &&
        !HasChange(view.change, LightingViewChange::FrameChanged) &&
        !HasChange(view.change, LightingViewChange::BodyChanged) &&
        std::memcmp(history.lighting.data(), light.data(), sizeof(history.lighting)) == 0;

    const std::array<u32, 32> constants{
        width,
        height,
        radianceLevelCount,
        steps,

        bits(view.forward.x),
        bits(view.forward.y),
        bits(view.forward.z),
        bits(
            static_cast<f32>(width) /
            static_cast<f32>(height)),

        bits(view.up.x),
        bits(view.up.y),
        bits(view.up.z),
        bits(std::tan(
            view.verticalFovRadians *
            0.5F)),

        bits(std::max(
            view.nearPlaneMeters,
            1.0e-5F)),
        bits(std::max(
            view.farPlaneMeters,
            view.nearPlaneMeters +
                1.0e-4F)),
        bits(std::max(
            settings.traceRadiusMeters,
            0.1F)),
        bits(std::max(
            settings.thicknessMeters,
            0.001F)),

        bits(std::clamp(
            settings.maximumScreenTraceRoughness,
            0.0F,
            1.0F)),
        bits(std::clamp(
            settings.mirrorRoughness,
            0.0F,
            1.0F)),
        bits(std::max(
            settings.cacheStrength,
            0.0F)),
        bits(quality),

        bits(static_cast<f32>(sdfOriginRelative.x)),
        bits(static_cast<f32>(sdfOriginRelative.y)),
        bits(static_cast<f32>(sdfOriginRelative.z)),
        bits(sdfAvailable ? sdf->voxelSize : 0.25F),

        bits(static_cast<f32>(exactOrigin.x)),
        bits(static_cast<f32>(exactOrigin.y)),
        bits(static_cast<f32>(exactOrigin.z)),
        bits(exactAvailable ? static_cast<f32>(exact->nodeCount) : 0.0F),

        bits(sdfAvailable ? static_cast<f32>(sdf->dimensions[0]) : 1.0F),
        bits(sdfAvailable ? static_cast<f32>(sdf->dimensions[1]) : 1.0F),
        bits(sdfAvailable ? static_cast<f32>(sdf->dimensions[2]) : 1.0F),
        bits(sdfAvailable ? 1.0F : 0.0F)
    };

    const bool useHardware = exactAvailable && settings.hardwareRayQueries &&
        hardwarePipeline_ != nullptr && exact->acceleration != nullptr;
    commands.SetComputePipeline(useHardware ? *hardwarePipeline_ : *pipeline_);
    if (useHardware) commands.SetComputeAccelerationStructure(0U, *exact->acceleration);
    commands.SetComputeConstants(
        constants);

    commands.SetComputeBuffer(
        0U,
        radianceCells);
    commands.SetComputeBuffer(
        1U,
        radianceLevels);
    commands.SetComputeBuffer(
        2U, sdfAvailable ? *sdf->distance : *dummySdf_);
    commands.SetComputeBuffer(
        3U, sdfAvailable ? *sdf->albedo : *dummySdf_);
    commands.SetComputeBuffer(
        4U, sdfAvailable ? *sdf->normal : *dummySdf_);
    commands.SetComputeBuffer(
        5U, sdfAvailable ? *sdf->radiance : *dummySdf_);

    commands.SetComputeBuffer(6U, exactAvailable ? *exact->triangles : *dummySdf_);
    commands.SetComputeBuffer(7U, exactAvailable ? *exact->nodes : *dummySdf_);
    commands.SetComputeBuffer(8U, *lightingBuffer);

    commands.Transition(*history.raw, rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    commands.SetComputeStorageTexture(
        0U,
        *history.raw);

    commands.SetComputeTexture(
        0U,
        sceneColor);
    commands.SetComputeTexture(
        1U,
        surfaceBaseRoughness);
    commands.SetComputeTexture(
        2U,
        surfaceNormalMetallic);
    commands.SetComputeTexture(
        3U,
        depth);
    commands.SetComputeTexture(
        4U,
        surfaceEmissionClass);

    commands.Dispatch(
        (width + 7U) / 8U,
        (height + 7U) / 8U,
        1U);
    commands.Transition(*history.raw, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
    const auto previous = history.index;
    const auto current = 1U - previous;
    const auto shift = view.cameraPositionInFrameMeters - history.view.cameraPositionInFrameMeters;
    const f32 aspect = static_cast<f32>(width) / static_cast<f32>(height);
    const std::array<math::Float4, 7> reconstruction{
        math::Float4{view.forward.x, view.forward.y, view.forward.z, aspect},
        math::Float4{view.up.x, view.up.y, view.up.z, std::tan(view.verticalFovRadians * 0.5F)},
        math::Float4{view.nearPlaneMeters, view.farPlaneMeters, 0.0F, 0.0F},
        math::Float4{history.view.forward.x, history.view.forward.y, history.view.forward.z, aspect},
        math::Float4{history.view.up.x, history.view.up.y, history.view.up.z, std::tan(history.view.verticalFovRadians * 0.5F)},
        math::Float4{history.view.nearPlaneMeters, history.view.farPlaneMeters, 0.0F, 0.0F},
        math::Float4{static_cast<f32>(shift.x), static_cast<f32>(shift.y), static_cast<f32>(shift.z), 0.0F}};
    auto reconstructionBuffer = device_.CreateBuffer({.sizeBytes = sizeof(reconstruction),
        .usage = rhi::BufferUsage::Structured, .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    std::memcpy(reconstructionBuffer->Map(), reconstruction.data(), sizeof(reconstruction));
    reconstructionBuffer->Unmap();
    commands.Transition(*history.radiance[current], rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    commands.Transition(*history.metadata[current], rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    commands.SetComputePipeline(*compositePipeline_);
    const std::array<u32, 4> compositeConstants{width, height, compatible ? 1U : 0U, settings.debugView};
    commands.SetComputeConstants(compositeConstants);
    commands.SetComputeBuffer(0U, *reconstructionBuffer);
    commands.SetComputeStorageTexture(0U, targetSceneColor);
    commands.SetComputeStorageTexture(1U, *history.radiance[current]);
    commands.SetComputeStorageTexture(2U, *history.metadata[current]);
    commands.SetComputeTexture(0U, *history.raw);
    commands.SetComputeTexture(1U, sceneColor);
    commands.SetComputeTexture(2U, surfaceBaseRoughness);
    commands.SetComputeTexture(3U, surfaceNormalMetallic);
    commands.SetComputeTexture(4U, depth);
    commands.SetComputeTexture(5U, *history.radiance[previous]);
    commands.SetComputeTexture(6U, *history.metadata[previous]);
    commands.Dispatch((width + 7U) / 8U, (height + 7U) / 8U, 1U);
    commands.Transition(*history.radiance[current], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
    commands.Transition(*history.metadata[current], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
    history.index = current; history.view = view; history.valid = settings.temporal;
    history.geometryRevision = geometryRevision; history.lastTick = tick_;
    std::copy_n(light.begin(), 4U, history.lighting.begin());
    parameters_.push_back({std::move(reconstructionBuffer), pendingFramesMask_});
    parameters_.push_back({std::move(lightingBuffer), pendingFramesMask_});
}
} // namespace orbit::lighting
