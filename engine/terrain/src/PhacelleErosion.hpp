#pragma once
// Adapted from the slope-guided Phacelle phase-noise erosion filter in
// ORBIT's v153 HTML playground, based on Rune Skovbo Johansen (2025).
// Mozilla Public License 2.0; https://mozilla.org/MPL/2.0/
// This derivative file remains subject to MPL-2.0.
// Procedural shape modification, not a physical sediment simulation.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace orbit::terrain::detail
{
struct PhacelleErosionSettings
{
    double strength{0.0};       // 0 preserves legacy terrain; 0.85 is the HTML preset
    double scale{1.0};          // frequency multiplier
    std::uint32_t octaves{4};   // 1..5, filtered against sample footprint
    double lowlandFadeMeters{2500.0};
};

namespace phacelle
{
struct V2 { float x, y; };
struct V3 { float x, y, z; };
inline V2 operator+(V2 a, V2 b) noexcept {return {a.x+b.x,a.y+b.y};}
inline V2 operator*(V2 a,float b) noexcept {return {a.x*b,a.y*b};}
inline float Clamp(float x,float a,float b) noexcept {return std::clamp(x,a,b);}
inline float Smooth(float t) noexcept {t=Clamp(t,0.f,1.f);return t*t*(3.f-2.f*t);}
inline float EaseOut(float t) noexcept {float v=1.f-Clamp(t,0.f,1.f);return 1.f-v*v;}
inline float SmoothStart(float t,float s) noexcept {if(s<=1e-10f)return std::max(0.f,t);if(t>=s)return t-.5f*s;return .5f*t*t/s;}
inline float PowInv(float t,float p) noexcept {return 1.f-std::pow(1.f-Clamp(t,0.f,1.f),p);}
inline std::uint32_t Avalanche(std::uint32_t v) noexcept
{
    v^=v>>16;v*=0x7feb352du;v^=v>>15;v*=0x846ca68bu;return v^(v>>16);
}
inline V2 Hash2(std::int32_t x,std::int32_t y) noexcept
{
    auto h=static_cast<std::uint32_t>(x)*0x9e3779b1u ^ static_cast<std::uint32_t>(y)*0x85ebca77u;
    return {static_cast<float>(Avalanche(h)&0xffffffu)/8388608.f-1.f,
            static_cast<float>(Avalanche(h^0x68bc21ebu)&0xffffffu)/8388608.f-1.f};
}
inline V3 Gradient(V2 p) noexcept
{
    auto ix=static_cast<std::int32_t>(std::floor(p.x)),iy=static_cast<std::int32_t>(std::floor(p.y));
    float x=p.x-std::floor(p.x),y=p.y-std::floor(p.y);
    float ux=x*x*x*(x*(x*6.f-15.f)+10.f),uy=y*y*y*(y*(y*6.f-15.f)+10.f);
    float dx=30.f*x*x*(x*(x-2.f)+1.f),dy=30.f*y*y*(y*(y-2.f)+1.f);
    V2 a=Hash2(ix,iy),b=Hash2(ix+1,iy),c=Hash2(ix,iy+1),d=Hash2(ix+1,iy+1);
    float va=a.x*x+a.y*y,vb=b.x*(x-1.f)+b.y*y,vc=c.x*x+c.y*(y-1.f),vd=d.x*(x-1.f)+d.y*(y-1.f);
    float q=va-vb-vc+vd;
    return {va+ux*(vb-va)+uy*(vc-va)+ux*uy*q,
            a.x+ux*(b.x-a.x)+uy*(c.x-a.x)+ux*uy*(a.x-b.x-c.x+d.x)+dx*(uy*q+vb-va),
            a.y+ux*(b.y-a.y)+uy*(c.y-a.y)+ux*uy*(a.y-b.y-c.y+d.y)+dy*(ux*q+vc-va)};
}
inline V3 Fbm(V2 p) noexcept
{
    V3 out{0,0,0};float freq=3.f,amp=1.f;
    for(int i=0;i<3;i++)
    {
        auto g=Gradient(p*freq);
        out.x+=g.x*amp;out.y+=g.y*amp*freq;out.z+=g.z*amp*freq;
        freq*=2.f;amp*=.1f;
    }
    return out;
}
inline V2 Phacelle(V2 p,V2 dir) noexcept
{
    constexpr float tau=6.2831853071795864769f;
    V2 transverse{-dir.y*.7f*tau,dir.x*.7f*tau};
    auto ix=static_cast<std::int32_t>(std::floor(p.x)),iy=static_cast<std::int32_t>(std::floor(p.y));
    float fx=p.x-std::floor(p.x),fy=p.y-std::floor(p.y);
    float re=0,im=0,total=0;
    for(int dy=-1;dy<=2;dy++)for(int dx=-1;dx<=2;dx++)
    {
        auto h=Hash2(ix+dx,iy+dy);
        float x=fx-dx-h.x*.5f,y=fy-dy-h.y*.5f;
        float weight=std::max(0.f,std::exp(-2.f*(x*x+y*y))-.01111f);
        if(weight<1e-8f)continue;
        float phase=x*transverse.x+y*transverse.y+.25f*tau;
        re+=std::cos(phase)*weight;im+=std::sin(phase)*weight;total+=weight;
    }
    if(total<1e-10f)return {0,0};
    re/=total;im/=total;
    float normalizer=std::max(.5f,std::sqrt(re*re+im*im));
    return {re/normalizer,im/normalizer};
}
inline float DetailWeight(float wavelength,float footprint) noexcept
{
    if(footprint<=0)return 1.f;
    if(wavelength<=2.f*footprint)return 0.f;
    if(wavelength>=4.f*footprint)return 1.f;
    float t=(wavelength-2.f*footprint)/(2.f*footprint);
    return t*t*t*(t*(t*6.f-15.f)+10.f);
}
inline float Chart(V2 p,float footprint,float scale,float erosionStrength,std::uint32_t octaves) noexcept
{
    V3 f=Fbm(p);
    float height=.125f*f.x*.5f+.5f,initialHeight=height;
    V2 slope0{f.y*.0625f,f.z*.0625f};
    float len=std::max(std::sqrt(slope0.x*slope0.x+slope0.y*slope0.y),1e-10f);
    V2 slope=slope0*(.7f/len);
    float fade=Clamp(f.x/.6f,-1.f,1.f);
    float combi=EaseOut(SmoothStart(len*1.25f,.01f*Clamp(fade+.5f,0.f,1.f)*1.25f));
    float baseScale=.15f*scale;
    float frequency=1.f/std::max(1e-4f,baseScale*.7f);
    float strength=.22f*baseScale*erosionStrength;
    float magnitude=0.f,roundingMultiplier=1.f;
    for(std::uint32_t k=0;k<octaves;k++)
    {
        float weight=DetailWeight(2700.f/scale/std::exp2(static_cast<float>(k)),footprint);
        if(weight<=0.f)break;
        float sl=std::sqrt(slope.x*slope.x+slope.y*slope.y);
        V2 dir=sl>1e-10f?slope*(1.f/sl):V2{0,0};
        auto cell=Phacelle(p*frequency,dir);
        V2 side{dir.y*(.7f*6.28318530718f)*frequency,-dir.x*(.7f*6.28318530718f)*frequency};
        float signCell=cell.y<0.f?-1.f:1.f;
        slope.x+=signCell*side.x*strength*.5f;
        slope.y+=signCell*side.y*strength*.5f;
        float faded=fade*(1.f-combi)+cell.x*.5f*combi;
        height+=faded*strength*weight;
        magnitude+=strength*weight;
        fade=faded;
        float rounding=.1f*Clamp(cell.x+.5f,0.f,1.f)*roundingMultiplier;
        combi=PowInv(combi,1.5f)*EaseOut(SmoothStart(std::abs(cell.y)*1.25f,rounding*1.25f));
        strength*=.5f;frequency*=2.f;roundingMultiplier*=2.f;
    }
    return (height-initialHeight-.65f*magnitude)*85000.f*.45f;
}
} // namespace phacelle

// Stable world-space charts and smooth triplanar weights avoid face seams.
// Input is unit direction, radius and spatial footprint (metres).
inline double PhacelleErosionDeltaMeters(
    const std::array<double,3>& direction,double planetRadiusMeters,
    double footprintMeters,const PhacelleErosionSettings& recipe) noexcept
{
    if(recipe.strength<=0.0||recipe.octaves==0)return 0.0;
    namespace p=phacelle;
    float scale=static_cast<float>(recipe.scale);
    float n[3]={static_cast<float>(direction[0]),static_cast<float>(direction[1]),static_cast<float>(direction[2])};
    float w[3]={std::pow(std::abs(n[0]),4.f),std::pow(std::abs(n[1]),4.f),std::pow(std::abs(n[2]),4.f)};
    float sum=std::max(1e-15f,w[0]+w[1]+w[2]),total=0;
    for(float& a:w){a*=p::Smooth(a/sum/.003f);total+=a;}
    float m=static_cast<float>(planetRadiusMeters/18000.0);
    float footprint=static_cast<float>(footprintMeters);
    const p::V2 charts[3]={{n[1]*m,n[2]*m},{n[0]*m,n[2]*m},{n[0]*m,n[1]*m}};
    float delta=0;
    for(int axis=0;axis<3;axis++)if(w[axis]>1e-12f)
        delta+=(w[axis]/std::max(total,1e-15f))*
            p::Chart(charts[axis],footprint,scale,static_cast<float>(recipe.strength),
                std::min(recipe.octaves,5u));
    return static_cast<double>(delta);
}
} // namespace orbit::terrain::detail
