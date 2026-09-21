#include <orbit/celestial_giants/GiantAppearance.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace orbit::celestial_giants
{
namespace
{
[[nodiscard]] math::Double3 FaceDirection(
    const u32 face,
    const f64 u,
    const f64 v)
{
    math::Double3 p{};
    switch (face)
    {
    case 0: p={ 1.0, v,-u}; break;
    case 1: p={-1.0, v, u}; break;
    case 2: p={ u, 1.0,-v}; break;
    case 3: p={ u,-1.0, v}; break;
    case 4: p={ u, v, 1.0}; break;
    default:p={-u, v,-1.0}; break;
    }
    return math::Normalize(p);
}

[[nodiscard]] u64 Mix(u64 x) noexcept
{
    x+=0x9e3779b97f4a7c15ULL;
    x=(x^(x>>30U))*0xbf58476d1ce4e5b9ULL;
    x=(x^(x>>27U))*0x94d049bb133111ebULL;
    return x^(x>>31U);
}

[[nodiscard]] f64 UnitHash(const u64 v) noexcept
{
    return static_cast<f64>(Mix(v)&0x00ffffffULL)/
        static_cast<f64>(0x00ffffffULL);
}

[[nodiscard]] f64 Noise(
    const math::Double3 p,
    const f64 scale,
    const u64 seed) noexcept
{
    const auto q=p*scale;
    const i64 x=static_cast<i64>(std::floor(q.x*4096.0));
    const i64 y=static_cast<i64>(std::floor(q.y*4096.0));
    const i64 z=static_cast<i64>(std::floor(q.z*4096.0));
    u64 h=Mix(seed^static_cast<u64>(x));
    h=Mix(h^static_cast<u64>(y));
    h=Mix(h^static_cast<u64>(z));
    return UnitHash(h);
}

[[nodiscard]] f64 Fractal(
    const math::Double3 p,
    const f64 scale,
    const u64 seed) noexcept
{
    f64 sum=0.0, weight=0.0, amp=1.0, freq=scale;
    for(u32 octave=0U;octave<4U;++octave)
    {
        sum+=Noise(p,freq,seed+octave*0x9e3779b9ULL)*amp;
        weight+=amp;
        amp*=0.5;
        freq*=2.03;
    }
    return weight>0.0?sum/weight:0.5;
}

[[nodiscard]] math::Double3 Lerp(
    const math::Double3 a,
    const math::Double3 b,
    const f64 t) noexcept
{
    return a*(1.0-t)+b*t;
}

[[nodiscard]] f64 SmoothStep(
    const f64 a,
    const f64 b,
    const f64 x) noexcept
{
    const f64 t=std::clamp((x-a)/(b-a),0.0,1.0);
    return t*t*(3.0-2.0*t);
}

[[nodiscard]] math::Double3 StormCenter(
    const u64 seed,
    const u32 index) noexcept
{
    const f64 longitude=
        (UnitHash(seed+index*17U)*2.0-1.0)*
        std::numbers::pi_v<f64>;
    const f64 latitude=
        (UnitHash(seed+index*31U+7U)*2.0-1.0)*
        0.55;
    const f64 c=std::cos(latitude);
    return {
        c*std::cos(longitude),
        std::sin(latitude),
        c*std::sin(longitude)
    };
}

void Validate(
    const GiantAppearanceParameters& p,
    const GiantAppearanceConfig& c)
{
    const auto colorOk=[](const math::Double3 v)
    {
        return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&
            v.x>=0.0&&v.y>=0.0&&v.z>=0.0;
    };

    if(c.faceResolution<3U ||
       !colorOk(p.baseColorLinear) ||
       !colorOk(p.bandColorLinear) ||
       !colorOk(p.polarColorLinear) ||
       !std::isfinite(p.bandFrequency) || p.bandFrequency<1.0 ||
       !std::isfinite(p.bandStrength) || p.bandStrength<0.0 || p.bandStrength>1.0 ||
       !std::isfinite(p.zonalShear) || p.zonalShear<0.0 ||
       !std::isfinite(p.stormStrength) || p.stormStrength<0.0 || p.stormStrength>1.0 ||
       !std::isfinite(p.stormScale) || p.stormScale<=0.0 ||
       !std::isfinite(p.polarStrength) || p.polarStrength<0.0 || p.polarStrength>1.0 ||
       !std::isfinite(p.depthContrast) || p.depthContrast<0.0 || p.depthContrast>1.0 ||
       !std::isfinite(p.turbulenceStrength) || p.turbulenceStrength<0.0 || p.turbulenceStrength>1.0 ||
       !std::isfinite(p.turbulenceScale) || p.turbulenceScale<=0.0)
        throw std::invalid_argument("Giant appearance parameters are invalid.");
}
} // namespace

math::Double3 EvaluateGiantColor(
    const GiantAppearanceParameters& p,
    math::Double3 d) noexcept
{
    d=math::Normalize(d);
    const f64 latitude=std::asin(std::clamp(d.y,-1.0,1.0));
    const f64 longitude=std::atan2(d.z,d.x);

    const f64 broad=
        Fractal(d,3.2,p.seed^0x42524f4144ULL)-0.5;
    const f64 fine=
        Fractal(d,p.turbulenceScale,p.seed^0x54555242ULL)-0.5;

    const f64 shear=
        p.zonalShear*
        (0.55*broad+0.45*fine)*
        std::cos(latitude);

    const f64 wave=
        std::sin(
            latitude*p.bandFrequency*
                std::numbers::pi_v<f64>+
            longitude*0.18+
            shear*4.0);

    const f64 narrow=
        std::sin(
            latitude*p.bandFrequency*
                2.07*
                std::numbers::pi_v<f64>-
            longitude*0.11+
            fine*1.7);

    f64 band=
        0.5+0.5*
        (0.72*wave+0.28*narrow);

    band=std::clamp(
        0.5+(band-0.5)*(0.35+1.3*p.bandStrength),
        0.0,1.0);

    const f64 depth=
        std::clamp(
            0.5+
            p.depthContrast*
                (0.65*broad+0.35*fine),
            0.0,1.0);

    math::Double3 color=
        Lerp(
            p.baseColorLinear,
            p.bandColorLinear,
            band);

    const f64 depthScale=
        1.0+
        (depth-0.5)*
        0.55;
    color=color*depthScale;

    // Deterministic oval-vortex approximation. Longitudinal stretching is
    // stronger than meridional stretching to mimic giant-planet storms.
    for(u32 storm=0U;storm<3U;++storm)
    {
        const auto center=
            StormCenter(
                p.seed^0x53544f524dULL,
                storm);
        const f64 angular=
            std::acos(
                std::clamp(
                    math::Dot(d,center),
                    -1.0,1.0));
        const f64 radius=
            (0.11+
             0.07*UnitHash(
                 p.seed+storm*101U))/
            std::max(
                std::sqrt(p.stormScale),
                0.5);
        const f64 mask=
            1.0-
            SmoothStep(
                radius,
                radius*2.0,
                angular);

        if(mask>0.0)
        {
            const f64 bright=
                (storm%2U)==0U?1.0:-0.45;
            const f64 strength=
                mask*p.stormStrength;
            color=
                color*
                (1.0+
                 bright*0.32*strength);
        }
    }

    const f64 polar=
        std::pow(
            std::abs(d.y),
            p.giantClass==GiantClass::IceGiant
                ? 2.0
                : 3.2)*
        p.polarStrength;

    color=
        Lerp(
            color,
            p.polarColorLinear,
            std::clamp(polar,0.0,1.0));

    if(p.giantClass==GiantClass::IceGiant)
    {
        // Ice giants are visually smoother and methane-dominated in the
        // baseline; the authored colors remain authoritative.
        color=
            Lerp(
                color,
                p.baseColorLinear,
                0.18);
    }

    return {
        std::max(color.x,0.0),
        std::max(color.y,0.0),
        std::max(color.z,0.0)
    };
}

u64 GiantAppearanceFingerprint(
    const GiantAppearanceParameters& p,
    const GiantAppearanceConfig& c)
{
    Validate(p,c);
    u64 h=0x4d32374749414e54ULL;
    const auto add=[&](u64 v){h=terrain::StableCombine64(h,v);};
    const auto addf=[&](f64 v){add(std::bit_cast<u64>(v));};
    add(static_cast<u64>(p.giantClass)); add(p.seed);
    for(const f64 v:{p.baseColorLinear.x,p.baseColorLinear.y,p.baseColorLinear.z,
        p.bandColorLinear.x,p.bandColorLinear.y,p.bandColorLinear.z,
        p.polarColorLinear.x,p.polarColorLinear.y,p.polarColorLinear.z,
        p.bandFrequency,p.bandStrength,p.zonalShear,p.stormStrength,
        p.stormScale,p.polarStrength,p.depthContrast,
        p.turbulenceStrength,p.turbulenceScale})
        addf(v);
    add(c.faceResolution);
    return h;
}

celestial_appearance::PlanetaryAppearanceProduct
BuildGiantAppearance(
    const GiantAppearanceParameters& p,
    const GiantAppearanceConfig& c)
{
    const u64 fingerprint=
        GiantAppearanceFingerprint(p,c);

    celestial_appearance::PlanetaryAppearanceProduct result;
    result.faceResolution=c.faceResolution;
    result.sampleFootprintMeters=1.0;
    result.fingerprint=fingerprint;
    result.texels.resize(
        static_cast<std::size_t>(6U)*
        c.faceResolution*c.faceResolution);

    for(u32 face=0U;face<6U;++face)
    for(u32 y=0U;y<c.faceResolution;++y)
    for(u32 x=0U;x<c.faceResolution;++x)
    {
        const f64 u=-1.0+2.0*static_cast<f64>(x)/(c.faceResolution-1U);
        const f64 v=-1.0+2.0*static_cast<f64>(y)/(c.faceResolution-1U);
        const auto d=FaceDirection(face,u,v);
        const auto color=EvaluateGiantColor(p,d);

        auto& texel=result.texels[
            static_cast<std::size_t>(face)*
                c.faceResolution*c.faceResolution+
            static_cast<std::size_t>(y)*
                c.faceResolution+x];

        texel.albedoLinear={
            static_cast<f32>(color.x),
            static_cast<f32>(color.y),
            static_cast<f32>(color.z)};
        texel.normal={
            static_cast<f32>(d.x),
            static_cast<f32>(d.y),
            static_cast<f32>(d.z)};
        texel.roughness=
            p.giantClass==GiantClass::IceGiant
                ? 0.92F
                : 0.88F;
        texel.directLightTransmittance=1.0F;
    }

    return result;
}
} // namespace orbit::celestial_giants
