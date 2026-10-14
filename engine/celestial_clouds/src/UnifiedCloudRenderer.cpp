#include <orbit/celestial_clouds/UnifiedCloudRenderer.hpp>
#include <orbit/celestial_clouds/UnifiedCloudNoise.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
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
constexpr const char* kMediumPart1 = R"(
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
struct Config
{
    float4 cameraAspect; // body-fixed eye xyz, aspect
    float4 forwardFov;   // view forward, tan half vertical fov
    float4 upNear;       // view up, near plane
    float4 sunFar;       // Sun direction, far plane
    float4 shell;        // planet radius, cloud base and top, cubemap face size
    float4 material;     // layer count, receiver shadows, live storm, irradiance
    float4 cacheAnchor;  // body-fixed unit tangent anchor xyz, generation w
    float4 cacheState;   // slice phase, cache active, inverse target height, star angular radius
};
[[vk::push_constant]] Config g;
[[vk::binding(0,0)]] ByteAddressBuffer g_weather;
// Authoritative GPU simulation export, optional. Only the input buffer is
// shared with A; B uses its own storm sampling and optical integration.
[[vk::binding(1,0)]] StructuredBuffer<float> g_storm;
#ifdef B3_WRITE_CACHE
[[vk::binding(2,0)]] RWByteAddressBuffer g_lightCache;
#else
[[vk::binding(2,0)]] ByteAddressBuffer g_lightCache;
#endif
// System B's independent CPU-baked tileable 32^3 RGBA cellular field.
[[vk::binding(3,0)]] ByteAddressBuffer g_densityNoise;
[[vk::binding(4,0)]] [[vk::combinedImageSampler]] Texture2D g_depth;
[[vk::binding(4,0)]] [[vk::combinedImageSampler]] SamplerState g_depthSampler;

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
)";

constexpr const char* kMediumPart2 = R"(    const float halfX = 0.5*g_storm[17]*g_storm[14];
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
// An independent, multi-scale 3D density representation for System B.
// The baked volume is periodic; non-commensurate rotated coordinates at
// different frequencies prevent grid-aligned repeats on planetary surfaces.
float4 NoiseVoxelB(int3 cell)
{
    const uint3 p=uint3(cell)&31u;
    return asfloat(g_densityNoise.Load4(
        ((p.z*32u+p.y)*32u+p.x)*16u));
}
float4 SampleNoiseB(float3 p)
{
    const float3 base=floor(p);
    const int3 c=int3(base);
    const float3 f=p-base;
    const float3 t=f*f*(3.-2.*f);
    const float4 n000=NoiseVoxelB(c);
    const float4 n100=NoiseVoxelB(c+int3(1,0,0));
    const float4 n010=NoiseVoxelB(c+int3(0,1,0));
    const float4 n110=NoiseVoxelB(c+int3(1,1,0));
    const float4 n001=NoiseVoxelB(c+int3(0,0,1));
    const float4 n101=NoiseVoxelB(c+int3(1,0,1));
    const float4 n011=NoiseVoxelB(c+int3(0,1,1));
    const float4 n111=NoiseVoxelB(c+int3(1,1,1));
    return lerp(
        lerp(lerp(n000,n100,t.x),lerp(n010,n110,t.x),t.y),
        lerp(lerp(n001,n101,t.x),lerp(n011,n111,t.x),t.y),
        t.z);
}
float3 RotateNoiseB(float3 p)
{
    return float3(
        0.734*p.x-0.427*p.y+0.528*p.z,
        0.427*p.x+0.891*p.y+0.151*p.z,
       -0.528*p.x+0.151*p.y+0.836*p.z);
}

float4 Density(float3 position, bool receiver,
    float footprintMeters, bool visibleDetail)
{
    if(receiver && g.material.y<0.5) return float4(0.,0.,0.,0.);
    const float radius=length(position);
    if(radius<=g.shell.x) return 0.;
    const float altitude=radius-g.shell.x;
    const float3 dir=position/radius;

    // Weather is authored on a ~50 km cubemap. Low-frequency directional
    // warp removes bilinear grid outlines without touching the storm's
    // physical coordinate system or introducing camera-dependent motion.
    const float3 warp=float3(
        sin(dir.y*47.+0.7)+0.35*sin(dir.z*109.+2.1),
        sin(dir.z*53.+1.4)+0.35*sin(dir.x*117.+0.8),
        sin(dir.x*43.+2.9)+0.35*sin(dir.y*113.+1.6));
    uint face;
    float2 cube;
    CubeCoords(normalize(dir+warp*0.0034),face,cube);
    const uint size=max(2u,(uint)g.shell.w);
    const float2 coord=saturate(cube*0.5+0.5)*float(size-1u);
    const uint layers=(uint)g.material.x;
    if(layers==0u || altitude>g.shell.z ||
        (altitude<g.shell.y && g.material.z<0.5)) return 0.;

    // Single-layer planets are common. Reject entirely empty columns
    // BEFORE touching the 3D noise buffer: the Sun cache queries many
    // clear locations and should not pay eight noise loads for each.
    float4 firstLayerWeather=0.;
    bool hasFirstWeather=false;
    if(layers==1u && g_storm[0]<0.5)
    {
        float4 firstShell,firstOptics;
        LayerDescriptor(0u,firstShell,firstOptics);
        if(altitude<=firstShell.x || altitude>=firstShell.y ||
            (receiver && firstOptics.w<0.5)) return 0.;
        firstLayerWeather=WeatherLayer(0u,face,coord,size);
        hasFirstWeather=true;
        const float coverage0=saturate(
            (firstLayerWeather.x-
             (0.52-firstShell.w*0.35-0.26))/0.52);
        if(coverage0<0.015 && firstLayerWeather.y<0.015)
            return 0.;
    }
    float other=0.,conv=0.;
    float otherScatter=0.,convScatter=0.;
    float otherPhase=0.,convPhase=0.;

    const float footprint=max(1.,footprintMeters);
    // Broad cellular features are 15-40 km, meso billows ~3-12 km,
    // and fine 0.3-1 km detail fades below its projected pixel footprint.
    const float3 rotated=RotateNoiseB(position);
    const float coarse=SampleNoiseB(
        rotated/4600. + float3(9.,17.,29.)).r;
    const float mesoFade=smoothstep(1200.,11000.,footprint);
    float meso=0.5;
    if(visibleDetail && mesoFade<0.999)
        meso=SampleNoiseB(
            position/1750. + float3(21.,11.,37.)).g;
    const float mesoContribution=(visibleDetail?
        1.-mesoFade:0.);
    const float baseValue=lerp(coarse,
        lerp(coarse,meso,0.42),mesoContribution);

    // Fine erosion only at resolvable projected footprints. Never make
    // unresolvable octaves alias into the regular "pebble" pattern.
    const float erosionFade=smoothstep(130.,1450.,footprint);
    float erosion=0.5;
    if(visibleDetail && erosionFade<0.999)
        erosion=SampleNoiseB(
            rotated/380. + float3(6.,47.,13.)).a;
    const float detailWeight=visibleDetail?(1.-erosionFade):0.;

    [loop]
    for(uint layer=0u;layer<layers;++layer)
    {
        float4 shell,optics;
        LayerDescriptor(layer,shell,optics);
        if(altitude<=shell.x || altitude>=shell.y) continue;
        if(receiver && optics.w<0.5) continue;
        const float4 w=(layer==0u && hasFirstWeather)?
            firstLayerWeather:WeatherLayer(layer,face,coord,size);
        const float coverage=saturate(
            (w.x-(0.52-shell.w*0.35-0.26))/0.52);
        if(coverage<0.015 && w.y<0.015) continue;
        const float type=saturate(w.w);
        const float tower=smoothstep(0.52,0.94,type);
        const float lowToFair=smoothstep(0.18,0.52,type);
        const float cloudTop=lerp(0.20,0.49,lowToFair);
        const float topHeight=lerp(cloudTop,0.94,tower)*
            lerp(0.89,1.08,coarse);
        const float h=(altitude-shell.x)/
            max(shell.y-shell.x,1.);
        const float hn=h/max(topHeight,0.10);
        const float bottom=smoothstep(0.,0.075,hn);
        const float top=1.-smoothstep(
            lerp(0.70,0.80,tower),1.,hn);
        // Tall updrafts narrow toward the crown; the anvil is another
        // medium component rather than an extension of the tower stem.
)";

constexpr const char* kMediumPart3 = R"(        const float taper=tower*0.24*
            pow(saturate(hn),1.4);
        const float cloudThreshold=lerp(
            0.73,0.32,saturate(coverage)) + taper;
        const float edgeWidth=0.07 + 0.03*mesoFade;
        const float shiftedBase=baseValue-
            detailWeight*0.15*(1.-erosion);
        const float shape=smoothstep(
            cloudThreshold-edgeWidth,
            cloudThreshold+edgeWidth,shiftedBase);
        const float profile=bottom*top/
            max(0.30,topHeight);
        // Upper cirrus and detached convective anvil outflow use a
        // thinner, broader optical density than the dense low layer.
        const float cirrus=saturate(w.y)*
            smoothstep(0.65,0.79,h)*
            (1.-smoothstep(0.92,1.,h))*
            smoothstep(0.35,0.75,baseValue);
        const float sigma=max(0.,shell.z)/
            max(shell.y-shell.x,1.)*
            (shape*profile*2.6+cirrus*0.30);
        const float cv=smoothstep(0.54,0.82,type);
        const float scattering=sigma*saturate(optics.y);
        const float phase=scattering*clamp(
            optics.z,-0.98,0.98);
        other+=sigma*(1.-cv);
        conv+=sigma*cv;
        otherScatter+=scattering*(1.-cv);
        convScatter+=scattering*cv;
        otherPhase+=phase*(1.-cv);
        convPhase+=phase*cv;
    }
    const float2 sim=StormMedium(position,radius);
    const float storm=sim.x;
    const float influence=sim.y;
    // A simulated convective medium replaces the corresponding authored
    // convection; independent fog and cirrus remain additive.
    const float total=other+lerp(conv,storm,influence);
    const float totalScatter=otherScatter+
        lerp(convScatter,0.995*storm,influence);
    const float phase=otherPhase+
        lerp(convPhase,0.72*0.995*storm,influence);
    return float4(max(total,0.),
        max(totalScatter,0.),phase,0.);
}

// Local optical-depth volume. Each cell corresponds to a fixed point on the
// planet within the anchor's tangent frame (not a camera-relative floating
// origin). Existing generation tags prevent stale data from becoming valid.
static const uint kCacheSide = 32u;
static const uint kCacheHeight = 16u;
static const float kCacheCellMeters = 2000.0;
void CacheAxes(out float3 east, out float3 north)
{
    const float3 a = normalize(g.cacheAnchor.xyz);
    const float3 reference = abs(a.y) < 0.92 ? float3(0,1,0) : float3(1,0,0);
    east = normalize(cross(reference,a));
    north = normalize(cross(a,east));
}
uint CacheIndex(uint3 q)
{
    return ((q.z*kCacheSide+q.y)*kCacheSide+q.x)*16u;
}
float3 CachePoint(uint3 q)
{
    float3 east,north;
    CacheAxes(east,north);
    const float2 metres =
        (float2(q.xy)+0.5-float(kCacheSide)*0.5)*kCacheCellMeters;
    const float altitude = (float(q.z)+0.5) /
        float(kCacheHeight)*max(g.shell.z,1000.);
    const float3 d = normalize(g.cacheAnchor.xyz +
        (east*metres.x+north*metres.y)/max(g.shell.x,1.));
    return d*(g.shell.x+altitude);
}
// Cloud sunlight is an optical integral over the occupied shell intervals.
// Six samples uniformly spaced along an entire grazing ray can miss the
// cloud layer or cause black/white radial aliases at orbital distance.
float SunDepthInterval(float3 p,float3 sun,float beginT,float endT,uint steps)
{
    if(endT<=beginT || steps==0u) return 0.;
    const float dt=(endT-beginT)/float(steps);
    float tau=0.;
    [loop]
    for(uint j=0u;j<steps;++j)
    {
        tau+=Density(p+sun*(beginT+(float(j)+0.5)*dt),
            false,max(850.,dt*0.15),false).x*dt;
    }
    return tau;
}
float SunDepthDirect(float3 p, uint requestedSteps)
{
    const float3 sun=normalize(g.sunFar.xyz);
    float outerNear,outerFar;
    if(!SphereInterval(p,sun,g.shell.x+g.shell.z,outerNear,outerFar))
        return 0.;
    const float endT=min(max(outerFar,0.),1000000.);
    if(endT<=0.) return 0.;
    float groundNear,groundFar;
    if(SphereInterval(p,sun,g.shell.x,groundNear,groundFar))
    {
        if((groundNear>0.01 && groundNear<endT) ||
           (groundNear<=0.01 && groundFar>0.01)) return 24.;
    }
    // With a live storm, participating cloud density may extend down
    // to the planet. Otherwise, the gap below the lowest authored
    // cloud layer is not part of the sunlight integration domain.
    const float innerRadius=g.shell.x+
        (g.material.z>0.5?0.:max(g.shell.y,0.));
    const uint budget=clamp(requestedSteps,2u,12u);
    float innerNear,innerFar;
    if(SphereInterval(p,sun,innerRadius,innerNear,innerFar))
    {
        if(innerNear<=0. && innerFar>0.)
            return min(max(SunDepthInterval(p,sun,
                min(innerFar,endT),endT,budget),0.),24.);
        if(innerNear>0. && innerNear<endT)
        {
            const float farStart=min(innerFar,endT);
            const float nearLen=innerNear;
            const float farLen=max(0.,endT-farStart);
            if(farLen<=0.)
                return min(max(SunDepthInterval(
                    p,sun,0.,innerNear,budget),0.),24.);
            const uint nearSteps=clamp(
                (uint)round(float(budget)*nearLen/
                    max(nearLen+farLen,1.)),1u,budget-1u);
            float tau=SunDepthInterval(
                p,sun,0.,innerNear,nearSteps);
            tau+=SunDepthInterval(
                p,sun,farStart,endT,budget-nearSteps);
            return min(max(tau,0.),24.);
        }
    }
    return min(max(SunDepthInterval(p,sun,0.,endT,budget),0.),24.);
}
#ifndef B3_WRITE_CACHE
bool CachedSunDepth(float3 p,out float opticalDepth)
{
    opticalDepth=0.;
    if(g.cacheState.y<0.5 || dot(g.cacheAnchor.xyz,g.cacheAnchor.xyz)<0.95)
        return false;
    float3 east,north;
    CacheAxes(east,north);
    const float3 dir=normalize(p);
    const float2 xy=float2(dot(dir,east),dot(dir,north))*g.shell.x;
    const float z=(length(p)-g.shell.x) /
        max(g.shell.z,1000.)*float(kCacheHeight)-0.5;
    const float2 horizontal=xy/kCacheCellMeters+float(kCacheSide)*0.5-0.5;
    const float3 q=float3(horizontal,z);
    if(any(q<0.) || q.x>=float(kCacheSide-1u) ||
        q.y>=float(kCacheSide-1u) || q.z>=float(kCacheHeight-1u))
        return false;
    const uint3 first=(uint3)floor(q);
    const float3 weight=frac(q);
    float optical=0.;
    [unroll]
    for(uint dz=0u;dz<2u;++dz)
    {
        [unroll]
        for(uint dy=0u;dy<2u;++dy)
        {
            [unroll]
            for(uint dx=0u;dx<2u;++dx)
            {
                const uint3 c=first+uint3(dx,dy,dz);
                const float4 v=asfloat(g_lightCache.Load4(CacheIndex(c)));
                if(v.y != g.cacheAnchor.w || !isfinite(v.x))
                    return false;
                const float w=(dx==0u?1.-weight.x:weight.x)*
                    (dy==0u?1.-weight.y:weight.y)*
                    (dz==0u?1.-weight.z:weight.z);
                optical+=v.x*w;
            }
        }
    }
    opticalDepth=clamp(optical,0.,24.);
    return true;
}
#else
// The compute translation unit still parses the other entrypoints.
bool CachedSunDepth(float3 p,out float opticalDepth)
{
    opticalDepth=0.;
    return false;
)";

constexpr const char* kMediumPart4 = R"(}
[numthreads(8,8,1)]
void ClearSunCache(uint3 id : SV_DispatchThreadID)
{
    if (id.x>=kCacheSide || id.y>=kCacheSide || id.z>=kCacheHeight) return;
    g_lightCache.Store4(CacheIndex(id), uint4(0u,0u,0u,0u));
}
[numthreads(8,8,1)]
void UpdateSunCache(uint3 id : SV_DispatchThreadID)
{
    if(id.x>=kCacheSide || id.y>=kCacheSide ||
        g.cacheState.y<0.5) return;
    const uint z=(uint)g.cacheState.x;
    if(z>=kCacheHeight) return;
    const uint3 cell=uint3(id.xy,z);
    const float3 p=CachePoint(cell);
    const float tau=SunDepthDirect(p,12u);
    g_lightCache.Store4(CacheIndex(cell),
        asuint(float4(tau,g.cacheAnchor.w,0.,0.)));
}
#endif

float4 March(V input) : SV_Target0
{
    float3 direction = CameraRay(input.uv);
    float first, last;
    const float outer = g.shell.x + g.shell.z;
    if (!SphereInterval(g.cameraAspect.xyz, direction, outer, first, last))
        return float4(0,0,0,1);
    float groundA, groundB;
    // When the live storm export participates, its low-level inflow and
    // precipitation regions may extend below the first procedural deck.
    // Do not clip those cells away at the authored cloud base.
    const float innerRadius=g.shell.x+
        (g.material.z>0.5 ? 0. : max(0.,g.shell.y));
    if (SphereInterval(g.cameraAspect.xyz, direction, innerRadius, groundA, groundB))
    {
        if (groundA > 0.) last = min(last, groundA);
        else if (groundB > 0.) first = max(first,groundB);
    }
    first = max(first, 0.);
    last = min(last, SceneStop(input.uv, direction));
    if (last <= first)
        return float4(0,0,0,1);

    // Progressive, *complete* view-ray integration: uniform 64 steps
    // discarded near-field silhouette detail while overallocating the
    // same number of samples to the unimportant far end of long rays.
    // A monotonic quadratic schedule covers the ENTIRE intersected
    // segment, with 0.35x step width near the camera and 1.65x far away.
    // Unlike unbounded fine stepping it cannot silently truncate a long
    // tangent ray when the iteration budget is exhausted.
    const uint steps=128u;
    const float span=last-first;
    const float invSteps=1./float(steps);
    const uint2 pixel=uint2(input.position.xy);
    uint seed=pixel.x*747796405u+pixel.y*2891336453u+0x9e3779b9u;
    seed=((seed>>((seed>>28u)+4u))^seed)*277803737u;
    seed=(seed>>22u)^seed;
    const float jitter=0.35+0.30*(float(seed)*(1./4294967296.));
    float T=1.;
    float3 L=0.;
    const float3 sun=normalize(g.sunFar.xyz);
    [loop]
    for(uint i=0u;i<steps && T>0.004;++i)
    {
        const float u=float(i)*invSteps;
        const float v=float(i+1u)*invSteps;
        const float t0=first+span*u*(0.35+0.65*u);
        const float t1=first+span*v*(0.35+0.65*v);
        const float dt=t1-t0;
        const float cameraDistance=t0+jitter*dt;
        const float3 p=g.cameraAspect.xyz+direction*cameraDistance;
        const float footprint=max(2.,cameraDistance*
            2.*g.forwardFov.w*g.cacheState.z);
        const float4 mediumAtPoint=Density(
            p,false,footprint,true);
        float sigma = mediumAtPoint.x;
        if (sigma < 1.e-8) continue;
        // Solar elevation measured against the horizon of the spherical
        // reference body, not a perpetual (mu*0.5+0.5) daylight term.
        // A cloud above the surface can see slightly below its local
        // tangent horizon: account for this geometric horizon depression.
        const float radius = length(p);
        const float sunMu = dot(p/max(radius,1.),sun);
        const float planetOverRadius =
            saturate(g.shell.x/max(radius,1.));
        const float horizonMu =
            -sqrt(max(0.,1.-planetOverRadius*planetOverRadius));
        const float solarHalfAngle =
            clamp(g.cacheState.w,0.0005,0.20);
        const float sunDisc = smoothstep(
            horizonMu-solarHalfAngle,
            horizonMu+solarHalfAngle,sunMu);
        const float angle = dot(direction,sun);
        const float meanG = clamp(
            mediumAtPoint.z/max(mediumAtPoint.y,1.e-7),-0.95,0.95);
        const float d = max(1.+meanG*meanG-2.*meanG*angle,0.02);
        // Proper solid-angle normalization of Henyey-Greenstein.
        // Without 1/(4*pi), forward scattering was ~12.6x too bright.
        const float phase =
            (1.-meanG*meanG)/(12.5663706144*d*sqrt(d));
        const float albedo =
            saturate(mediumAtPoint.y/max(sigma,1.e-8));
        float tauSun=0.;
        if(!CachedSunDepth(p,tauSun))
            tauSun=SunDepthDirect(p,6u);
        const float irradiance = max(0.,g.material.w);
        const float sunlight = irradiance*sunDisc*phase*
            albedo*exp(-tauSun);

        // Temporary bounded skylight proxy until B owns an atmosphere-LUT
        // driven hemispherical irradiance / multiple-scattering cache.
        // Crucially: no permanent light on the unlit hemisphere, and no
        // unoccluded 0.13+0.20 source injected into opaque cloud interiors.
        const float twilight = smoothstep(
            -0.14,0.10,sunMu-horizonMu);
        const float height01 = saturate(
            (radius-g.shell.x-g.shell.y)/
            max(g.shell.z-g.shell.y,1.));
        const float skyAccess =
            (0.12+0.88*height01*height01)*
            exp(-0.20*min(tauSun,24.));
        const float skyAmbient =
            irradiance*0.055*twilight*skyAccess*albedo;
        const float alpha = 1.-exp(-sigma*dt);
        const float3 source =
            float3(1.0,0.96,0.91)*sunlight +
            float3(0.42,0.57,0.83)*skyAmbient;
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
        tau += Density(p,true,max(600.,delta*0.15),false).x * delta;
    }
    float visibility = exp(-tau);
    return float4(visibility,visibility,visibility,1);
}
)";
// Assemble the HLSL at runtime: MSVC rejects a single 20 KiB raw string
// (C2026), and adjacent C++ string literals would still be concatenated
// by the compiler. Each fragment stays below the MSVC literal limit.
// Do not change the fragment boundaries without updating the shader validator.
[[nodiscard]] std::string BuildMediumShaderSource()
{
    std::string source;
    source.reserve(sizeof(kMediumPart1) + sizeof(kMediumPart2) + sizeof(kMediumPart3) + sizeof(kMediumPart4) + 1U);
    source += kMediumPart1;
    source += kMediumPart2;
    source += kMediumPart3;
    source += kMediumPart4;
    return source;
}

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
    const std::string mediumSource = BuildMediumShaderSource();
    const auto vertex = compiler.Compile({
        .source = kVertex, .entryPoint = "main", .stage = shader::Stage::Vertex, .debug = false});
    // Orbit's Vulkan pipelines select SPIR-V entrypoint "main"; DXC
    // otherwise preserves the requested HLSL entrypoint name. Rename
    // only the selected System B entrypoint before compiling each stage.
    const std::string marchSource =
        std::string("#define March main\n") + mediumSource;
    const std::string shadowSource =
        std::string("#define ReceiverShadow main\n") + mediumSource;
    const auto pixel = compiler.Compile({
        .source = marchSource, .entryPoint = "main",
        .stage = shader::Stage::Pixel, .debug = false});
    const auto shadowPixel = compiler.Compile({
        .source = shadowSource, .entryPoint = "main",
        .stage = shader::Stage::Pixel, .debug = false});
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
    // Report WHICH B pipeline failed; a generic Vulkan exception previously
    // hid the distinction between march, receiver shadow and composite.
    const auto checkedPipeline = [&](const char* name, const auto& fragment,
        const u32 constants, const u32 buffers, const u32 textures,
        const rhi::BlendMode blend)
    {
        try
        {
            return pipeline(fragment, constants, buffers, textures, blend);
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error(
                std::string("Unified Cloud B ") + name + " pipeline: " + e.what());
        }
    };
    march_ = checkedPipeline("March", pixel, 32U, 4U, 1U, rhi::BlendMode::Opaque);
    receiverShadow_ = checkedPipeline("ReceiverShadow", shadowPixel,
        32U, 4U, 1U, rhi::BlendMode::Opaque);
    composite_ = checkedPipeline("Composite", compPixel,
        0U, 0U, 1U, rhi::BlendMode::Alpha);
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
    emptySunCache_ = device.CreateBuffer({
        .sizeBytes = 16ULL,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    if (!emptySunCache_)
        throw std::runtime_error("Unified System B inactive Sun cache allocation failed");
    std::memset(emptySunCache_->Map(), 0, 16U);
    emptySunCache_->Unmap();
    const auto independentNoise = BuildUnifiedCloudNoise();
    densityNoise_ = device.CreateBuffer({
        .sizeBytes = static_cast<u64>(independentNoise.size() * sizeof(f32)),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    if (!densityNoise_)
        throw std::runtime_error("Unified B 3D morphology noise buffer allocation failed");
    std::memcpy(densityNoise_->Map(), independentNoise.data(),
        independentNoise.size() * sizeof(f32));
    densityNoise_->Unmap();

    const std::string computeSource =
        std::string("#define B3_WRITE_CACHE 1\n") + mediumSource;
    const std::string updateSource =
        std::string("#define UpdateSunCache main\n") + computeSource;
    const auto compute = compiler.Compile({
        .source = updateSource, .entryPoint = "main",
        .stage = shader::Stage::Compute, .debug = false});
    sunCachePipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = compute.bytecode.data(), .size = compute.bytecode.size()},
        .pushConstantDwords = 32U,
        .shaderResourceBuffers = 4U,
        .storageTextures = 0U,
        .sampledTextures = 0U
    });
    if (!sunCachePipeline_)
        throw std::runtime_error("Unified System B Sun cache compute pipeline unavailable");
    const std::string clearSource =
        std::string("#define ClearSunCache main\n") + computeSource;
    const auto clearShader = compiler.Compile({
        .source = clearSource, .entryPoint = "main",
        .stage = shader::Stage::Compute, .debug = false});
    sunCacheClearPipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = clearShader.bytecode.data(),
            .size = clearShader.bytecode.size()},
        .pushConstantDwords = 32U,
        .shaderResourceBuffers = 4U,
        .storageTextures = 0U,
        .sampledTextures = 0U
    });
    if (!sunCacheClearPipeline_)
        throw std::runtime_error("Unified System B Sun cache clear pipeline unavailable");
}
UnifiedCloudRenderer::~UnifiedCloudRenderer() = default;

namespace
{
[[nodiscard]] std::array<u32,32> PackedInputs(
    const f64 radius, const CloudFieldProduct& source,
    const GpuCloudFieldProduct& field,
    const celestial_atmosphere::AtmosphereRenderView& view,
    const u32 width, const u32 height,
    const UnifiedCloudRenderer::SunCache* cache,
    const bool hasStorm)
{
    f64 base = std::numeric_limits<f64>::max();
    f64 top = 0.0;
    for (const auto& layer : source.layers)
    {
        base = std::min(base, layer.parameters.baseAltitudeMeters);
        top = std::max(top, layer.parameters.topAltitudeMeters);
    }
    // The active solver currently publishes 40 vertical cells of 500 m.
    if (hasStorm)
        top = std::max(top, 21000.0);
    const bool casts = std::any_of(source.layers.begin(),source.layers.end(),
        [](const auto& layer) { return layer.parameters.shadowParticipation; });
    const auto f=[](const f64 x) { return Bits(static_cast<f32>(x)); };
    return {
        f(view.cameraPositionMeters.x),f(view.cameraPositionMeters.y),
        f(view.cameraPositionMeters.z),
        Bits(static_cast<f32>(width)/static_cast<f32>(height)),
        Bits(view.forward.x),Bits(view.forward.y),Bits(view.forward.z),
        Bits(std::tan(view.verticalFovRadians*0.5F)),
        Bits(view.up.x),Bits(view.up.y),Bits(view.up.z),
        Bits(view.nearPlaneMeters),
        Bits(view.sunDirection.x),Bits(view.sunDirection.y),
        Bits(view.sunDirection.z),Bits(view.farPlaneMeters),
        f(radius),f(base),f(top),
        Bits(static_cast<f32>(field.FaceResolution())),
        Bits(static_cast<f32>(field.LayerCount())),
        Bits(casts?1.0F:0.0F),Bits(hasStorm?1.0F:0.0F),
        Bits(view.irradianceScale),
        f(cache && cache->anchored?cache->anchor.x:0.0),
        f(cache && cache->anchored?cache->anchor.y:0.0),
        f(cache && cache->anchored?cache->anchor.z:0.0),
        Bits(cache ? static_cast<f32>(cache->generation) : 0.0F),
        Bits(cache ? static_cast<f32>(cache->phase) : 0.0F),
        Bits(cache && cache->active?1.0F:0.0F),
        Bits(1.0F/static_cast<f32>(std::max(height,1U))),
        Bits(view.sunAngularRadiusRadians)
    };
}
} // namespace

std::unique_ptr<UnifiedCloudRenderer::SunCache>
UnifiedCloudRenderer::CreateSunCache(rhi::Device& device) const
{
    auto cache=std::make_unique<SunCache>();
    cache->buffer=device.CreateBuffer({
        .sizeBytes = 32ULL*32ULL*16ULL*16ULL,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::ShaderResource});
    if(!cache->buffer)
        throw std::runtime_error("Unified B sunlight cache allocation failed");
    return cache;
}
void UnifiedCloudRenderer::UpdateSunCache(
    rhi::CommandList& commands,
    SunCache& cache,
    GpuCloudFieldProduct& field,
    const CloudFieldProduct& source,
    const f64 referenceRadiusMeters,
    const celestial_atmosphere::AtmosphereRenderView& view,
    rhi::Buffer* const stormExport, const f64 simulationSeconds,
    const u64 bodyHigh, const u64 bodyLow)
{
    cache.active=false;
    if (!cache.buffer || source.layers.empty() ||
        field.LayerCount()!=source.layers.size()) return;
    const auto& position=view.cameraPositionMeters;
    const f64 length=std::sqrt(position.x*position.x+
        position.y*position.y+position.z*position.z);
    if (!(length>1.0) || !(referenceRadiusMeters>1.0)) return;
    const math::Double3 direction{position.x/length,
        position.y/length,position.z/length};
    bool reanchor=!cache.anchored;
    if (!reanchor)
    {
        const f64 dot=std::clamp(
            direction.x*cache.anchor.x+
            direction.y*cache.anchor.y+
            direction.z*cache.anchor.z,-1.0,1.0);
        reanchor=std::acos(dot)*referenceRadiusMeters>16000.0;
    }
    const f64 sunCos=static_cast<f64>(view.sunDirection.x)*cache.lastSun.x+
        static_cast<f64>(view.sunDirection.y)*cache.lastSun.y+
        static_cast<f64>(view.sunDirection.z)*cache.lastSun.z;
    const bool largeSunChange=cache.sunSet && sunCos<0.9998;
    const bool timeJump=cache.timeSet && (
        !std::isfinite(simulationSeconds) ||
        simulationSeconds+0.001<cache.lastSimulationSeconds ||
        simulationSeconds-cache.lastSimulationSeconds>60.0);
    const bool changed = reanchor || largeSunChange || timeJump ||
        !cache.bodySet || cache.bodyHigh != bodyHigh ||
        cache.bodyLow != bodyLow ||
        cache.sourceFingerprint!=field.Fingerprint() ||
        cache.lastStorm!=stormExport ||
        cache.lastRadius!=referenceRadiusMeters;
    if(changed)
    {
        cache.generation=cache.generation>=1000000U?1U:cache.generation+1U;
    }
    if(reanchor)
    {
        cache.anchor=direction;
        cache.anchored=true;
    }
    cache.bodyHigh=bodyHigh;
    cache.bodyLow=bodyLow;
    cache.bodySet=true;
    cache.sourceFingerprint=field.Fingerprint();
    cache.lastStorm=stormExport;
    cache.lastRadius=referenceRadiusMeters;
    cache.lastSun=view.sunDirection;
    cache.sunSet=true;
    cache.lastSimulationSeconds=simulationSeconds;
    cache.timeSet=true;
    cache.active=true;
    cache.phase=(cache.phase+1U)%16U;
    auto constants=PackedInputs(referenceRadiusMeters,source,field,view,
        1U,1U,&cache,stormExport != nullptr);
    commands.Transition(*cache.buffer,
        rhi::ResourceState::ShaderResource,
        rhi::ResourceState::UnorderedAccess);
    if (!cache.initialized)
    {
        // No reliance on undefined GPU allocation contents. Clear once,
        // then validate every read with explicit generation tags.
        commands.SetComputePipeline(*sunCacheClearPipeline_);
        commands.SetComputeConstants(constants);
        commands.SetComputeBuffer(0U,field.Buffer());
        commands.SetComputeBuffer(1U,
            stormExport ? *stormExport : *emptyStorm_);
        commands.SetComputeBuffer(2U,*cache.buffer);
        commands.SetComputeBuffer(3U,*densityNoise_);
        commands.Dispatch(4U,4U,16U);
        commands.UavBarrier(*cache.buffer);
        cache.initialized=true;
    }
    commands.SetComputePipeline(*sunCachePipeline_);
    commands.SetComputeConstants(constants);
    commands.SetComputeBuffer(0U,field.Buffer());
    commands.SetComputeBuffer(1U,
        stormExport ? *stormExport : *emptyStorm_);
    commands.SetComputeBuffer(2U,*cache.buffer);
    commands.SetComputeBuffer(3U,*densityNoise_);
    commands.Dispatch(4U,4U,1U);
    commands.UavBarrier(*cache.buffer);
    commands.Transition(*cache.buffer,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::ShaderResource);
}
void UnifiedCloudRenderer::DrawMedium(
    rhi::CommandList& commands, const bool shadow,
    rhi::Texture& depth, GpuCloudFieldProduct& field, rhi::Texture& target,
    const f64 bodyRadiusMeters, const CloudFieldProduct& source,
    const celestial_atmosphere::AtmosphereRenderView& view,
    rhi::Buffer* const stormExport, const SunCache* const sunCache)
{
    if (!target.Width() || !target.Height() ||
        field.FaceResolution()<2U || source.layers.empty() ||
        field.LayerCount()!=source.layers.size() ||
        field.FaceResolution()!=source.faceResolution) return;
    const auto constants=PackedInputs(bodyRadiusMeters,source,field,view,
        target.Width(),target.Height(),sunCache,stormExport != nullptr);
    commands.SetRenderTarget(target);
    commands.SetViewport({.x=0.0F,.y=0.0F,
        .width=static_cast<f32>(target.Width()),
        .height=static_cast<f32>(target.Height()),
        .minDepth=0.0F,.maxDepth=1.0F});
    commands.SetScissor({.left=0,.top=0,
        .right=static_cast<i32>(target.Width()),
        .bottom=static_cast<i32>(target.Height())});
    commands.SetGraphicsPipeline(shadow?*receiverShadow_:*march_);
    commands.SetGraphicsConstants(constants);
    commands.SetGraphicsBuffer(0,field.Buffer());
    commands.SetGraphicsBuffer(1,stormExport?*stormExport:*emptyStorm_);
    commands.SetGraphicsBuffer(2,
        sunCache && sunCache->active ? *sunCache->buffer : *emptySunCache_);
    commands.SetGraphicsBuffer(3,*densityNoise_);
    commands.SetGraphicsTexture(0,depth);
    commands.Draw(6);
}
void UnifiedCloudRenderer::Draw(
    rhi::CommandList& commands,rhi::Texture& depth,
    GpuCloudFieldProduct& field,rhi::Texture& target,
    const f64 radius,const CloudFieldProduct& source,
    const celestial_atmosphere::AtmosphereRenderView& view,
    rhi::Buffer* const stormExport,const SunCache* const sunCache)
{
    DrawMedium(commands,false,depth,field,target,radius,source,
        view,stormExport,sunCache);
}
void UnifiedCloudRenderer::DrawReceiverShadow(
    rhi::CommandList& commands,rhi::Texture& depth,
    GpuCloudFieldProduct& field,rhi::Texture& target,
    const f64 radius,const CloudFieldProduct& source,
    const celestial_atmosphere::AtmosphereRenderView& view,
    rhi::Buffer* const stormExport)
{
    // Receiver shadows still trace their own depth-bounded path.
    DrawMedium(commands,true,depth,field,target,radius,source,
        view,stormExport,nullptr);
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
