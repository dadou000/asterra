#pragma once
namespace orbit::lighting {
// Shared optical-depth ABI: a 64-byte header followed by tagged float4 voxels.
inline constexpr const char* kCloudSunShader = R"(
bool CloudSunTau(ByteAddressBuffer volume, float3 position, out float tau)
{
    tau = 0.0;
    const float4 anchorRadius = asfloat(volume.Load4(0u));
    const float4 cameraTop = asfloat(volume.Load4(16u));
    const float4 state = asfloat(volume.Load4(32u));
    if (state.y < 0.5 || anchorRadius.w <= 0.0) return false;
    const float radius = length(position);
    const float3 direction = position / max(radius, 1.0);
    if (dot(direction, anchorRadius.xyz) <= 0.1) return false;
    const float altitude = radius - anchorRadius.w;
    if (altitude >= cameraTop.w) return true;
    const float3 east = normalize(cross(float3(0,1,0), anchorRadius.xyz) + float3(1e-6,0,0));
    const float3 north = cross(anchorRadius.xyz, east);
    const float2 t = float2(dot(direction,east),dot(direction,north))*anchorRadius.w;
    const float3 cameraDirection = normalize(cameraTop.xyz);
    const float2 cameraT = float2(dot(cameraDirection,east),dot(cameraDirection,north))*anchorRadius.w;
    const float2 offset = abs(t-cameraT);
    uint cascade = 0u;
    float cell = 250.0;
    if (any(offset >= 46.0*cell)) { cascade=1u; cell=2000.0; }
    if (any(offset >= 46.0*cell)) { cascade=2u; cell=8000.0; }
    if (any(offset >= 46.0*cell)) return false;
    const float2 f=t/cell-0.5;
    const int2 base=int2(floor(f));
    const float2 w=frac(f);
    const float fk=clamp(altitude/max(cameraTop.w,1.0)*32.0-0.5,0.0,31.0);
    const uint k0=uint(floor(fk));
    const float wk=frac(fk);
    [unroll] for(uint dz=0u;dz<2u;++dz)
    [unroll] for(int dy=0;dy<2;++dy)
    [unroll] for(int dx=0;dx<2;++dx) {
        const int ai=base.x+dx, aj=base.y+dy;
        const uint layer=min(k0+dz,31u);
        const uint row=uint(aj+96*4096)%96u, col=uint(ai+96*4096)%96u;
        const uint address=64u+(((cascade*32u+layer)*96u+row)*96u+col)*16u;
        const float4 v=asfloat(volume.Load4(address));
        if(v.y!=float(ai)+100000.5 || v.z!=float(aj)+100000.5 || v.w!=state.x) return false;
        tau+=(dz==0u?1.0-wk:wk)*(dx==0?1.0-w.x:w.x)*(dy==0?1.0-w.y:w.y)*max(v.x,0.0);
    }
    return true;
}
)";
}
