#include <orbit/celestial_small_bodies/SmallBodyAppearance.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace orbit::celestial_small_bodies
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
    f64 sum=0.0;
    f64 weight=0.0;
    f64 amplitude=1.0;
    f64 frequency=scale;

    for(u32 octave=0U;octave<5U;++octave)
    {
        sum+=Noise(p,frequency,seed+octave*0x9e3779b9ULL)*amplitude;
        weight+=amplitude;
        amplitude*=0.5;
        frequency*=2.07;
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
    if(a==b) return x>=b?1.0:0.0;
    const f64 t=std::clamp((x-a)/(b-a),0.0,1.0);
    return t*t*(3.0-2.0*t);
}

[[nodiscard]] math::Double3 CraterCenter(
    const u64 seed,
    const u32 index) noexcept
{
    const f64 z=UnitHash(seed+index*41U)*2.0-1.0;
    const f64 azimuth=
        UnitHash(seed+index*73U+11U)*
        2.0*std::numbers::pi_v<f64>;
    const f64 r=std::sqrt(std::max(1.0-z*z,0.0));
    return {r*std::cos(azimuth),z,r*std::sin(azimuth)};
}

[[nodiscard]] f64 CraterRelief(
    const SmallBodyParameters& p,
    const math::Double3 d) noexcept
{
    f64 relief=0.0;
    const u32 count=
        static_cast<u32>(
            4.0+std::round(
                std::clamp(p.craterDensity,0.0,1.0)*20.0));

    for(u32 i=0U;i<count;++i)
    {
        const auto center=CraterCenter(p.seed^0x435241544552ULL,i);
        const f64 angle=
            std::acos(std::clamp(math::Dot(d,center),-1.0,1.0));
        const f64 radius=
            0.035+
            0.11*UnitHash(p.seed+i*137U+29U);
        const f64 normalized=angle/std::max(radius,1.0e-6);

        const f64 bowl=
            normalized<1.0
                ? -(1.0-normalized*normalized)
                : 0.0;
        const f64 rim=
            SmoothStep(1.35,1.0,normalized)-
            SmoothStep(1.0,0.86,normalized);

        relief +=
            bowl*p.craterDepth+
            rim*p.craterRimStrength;
    }

    return relief;
}

void Validate(
    const SmallBodyParameters& p,
    const SmallBodyAppearanceConfig& c)
{
    const auto finitePositive=[](const f64 v)
    {
        return std::isfinite(v)&&v>0.0;
    };
    const auto colorOk=[](const math::Double3 v)
    {
        return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&
            v.x>=0.0&&v.y>=0.0&&v.z>=0.0;
    };

    if(c.faceResolution<3U ||
       !finitePositive(p.axisScale.x) ||
       !finitePositive(p.axisScale.y) ||
       !finitePositive(p.axisScale.z) ||
       !std::isfinite(p.irregularity) || p.irregularity<0.0 || p.irregularity>0.65 ||
       !std::isfinite(p.largeLobeStrength) || p.largeLobeStrength<0.0 || p.largeLobeStrength>0.65 ||
       !std::isfinite(p.surfaceRoughness) || p.surfaceRoughness<0.0 || p.surfaceRoughness>1.0 ||
       !colorOk(p.regolithColorLinear) ||
       !colorOk(p.freshMaterialColorLinear) ||
       !std::isfinite(p.colorVariation) || p.colorVariation<0.0 || p.colorVariation>1.0 ||
       !std::isfinite(p.craterDensity) || p.craterDensity<0.0 || p.craterDensity>1.0 ||
       !std::isfinite(p.craterDepth) || p.craterDepth<0.0 || p.craterDepth>0.5 ||
       !std::isfinite(p.craterRimStrength) || p.craterRimStrength<0.0 || p.craterRimStrength>0.5 ||
       !std::isfinite(p.oppositionStrength) || p.oppositionStrength<0.0 || p.oppositionStrength>4.0 ||
       !finitePositive(p.oppositionWidthRadians) ||
       !std::isfinite(p.singleScatteringAlbedo) || p.singleScatteringAlbedo<0.0 || p.singleScatteringAlbedo>1.0 ||
       !std::isfinite(p.macroscopicRoughnessRadians) || p.macroscopicRoughnessRadians<0.0 ||
       p.macroscopicRoughnessRadians>1.4)
    {
        throw std::invalid_argument(
            "Small-body appearance parameters are invalid.");
    }
}
} // namespace

f32 SmallBodyShapeProduct::At(
    const u32 face,
    const u32 x,
    const u32 y) const
{
    if(face>=6U || x>=faceResolution || y>=faceResolution)
        throw std::out_of_range("Small-body shape sample is out of range.");

    return radiusScale[
        static_cast<std::size_t>(face)*faceResolution*faceResolution+
        static_cast<std::size_t>(y)*faceResolution+x];
}

f64 EvaluateRadiusScale(
    const SmallBodyParameters& p,
    math::Double3 d) noexcept
{
    d=math::Normalize(d);

    const f64 ellipsoid=
        1.0/std::sqrt(
            (d.x*d.x)/(p.axisScale.x*p.axisScale.x)+
            (d.y*d.y)/(p.axisScale.y*p.axisScale.y)+
            (d.z*d.z)/(p.axisScale.z*p.axisScale.z));

    const f64 broad=
        (Fractal(d,2.1,p.seed^0x534841504542ULL)-0.5)*2.0;
    const f64 medium=
        (Fractal(d,5.7,p.seed^0x53484150454dULL)-0.5)*2.0;

    const f64 lobe=
        std::sin(
            std::atan2(d.z,d.x)*2.0+
            d.y*2.7+
            UnitHash(p.seed)*6.0)*
        p.largeLobeStrength;

    const f64 relief=
        CraterRelief(p,d);

    return std::max(
        0.18,
        ellipsoid*
        (1.0+
         p.irregularity*(0.68*broad+0.32*medium)+
         lobe+
         relief));
}

u64 SmallBodyAppearanceFingerprint(
    const SmallBodyParameters& p,
    const SmallBodyAppearanceConfig& c)
{
    Validate(p,c);
    u64 h=0x4d3238534d414c4cULL;
    const auto add=[&](u64 v){h=terrain::StableCombine64(h,v);};
    const auto addf=[&](f64 v){add(std::bit_cast<u64>(v));};

    add(static_cast<u64>(p.bodyClass));
    add(p.seed);

    for(const f64 v:{
        p.axisScale.x,p.axisScale.y,p.axisScale.z,
        p.irregularity,p.largeLobeStrength,p.surfaceRoughness,
        p.regolithColorLinear.x,p.regolithColorLinear.y,p.regolithColorLinear.z,
        p.freshMaterialColorLinear.x,p.freshMaterialColorLinear.y,p.freshMaterialColorLinear.z,
        p.colorVariation,p.craterDensity,p.craterDepth,p.craterRimStrength,
        p.oppositionStrength,p.oppositionWidthRadians,p.singleScatteringAlbedo,
        p.macroscopicRoughnessRadians})
        addf(v);

    add(c.faceResolution);
    return h;
}

SmallBodyShapeProduct BuildSmallBodyShape(
    const SmallBodyParameters& p,
    const SmallBodyAppearanceConfig& c)
{
    const u64 fp=SmallBodyAppearanceFingerprint(p,c);
    SmallBodyShapeProduct result;
    result.faceResolution=c.faceResolution;
    result.fingerprint=fp;
    result.minimumRadiusScale=1.0e30;
    result.maximumRadiusScale=0.0;
    result.radiusScale.resize(
        static_cast<std::size_t>(6U)*c.faceResolution*c.faceResolution);

    for(u32 face=0U;face<6U;++face)
    for(u32 y=0U;y<c.faceResolution;++y)
    for(u32 x=0U;x<c.faceResolution;++x)
    {
        const f64 u=-1.0+2.0*static_cast<f64>(x)/(c.faceResolution-1U);
        const f64 v=-1.0+2.0*static_cast<f64>(y)/(c.faceResolution-1U);
        const f64 scale=EvaluateRadiusScale(p,FaceDirection(face,u,v));

        result.radiusScale[
            static_cast<std::size_t>(face)*c.faceResolution*c.faceResolution+
            static_cast<std::size_t>(y)*c.faceResolution+x]=
            static_cast<f32>(scale);

        result.minimumRadiusScale=std::min(result.minimumRadiusScale,scale);
        result.maximumRadiusScale=std::max(result.maximumRadiusScale,scale);
    }

    return result;
}

celestial_appearance::PlanetaryAppearanceProduct
BuildSmallBodyAppearance(
    const SmallBodyParameters& p,
    const SmallBodyAppearanceConfig& c)
{
    const u64 fp=SmallBodyAppearanceFingerprint(p,c);

    celestial_appearance::PlanetaryAppearanceProduct result;
    result.faceResolution=c.faceResolution;
    result.sampleFootprintMeters=1.0;
    result.fingerprint=fp;
    result.texels.resize(
        static_cast<std::size_t>(6U)*c.faceResolution*c.faceResolution);

    for(u32 face=0U;face<6U;++face)
    for(u32 y=0U;y<c.faceResolution;++y)
    for(u32 x=0U;x<c.faceResolution;++x)
    {
        const f64 u=-1.0+2.0*static_cast<f64>(x)/(c.faceResolution-1U);
        const f64 v=-1.0+2.0*static_cast<f64>(y)/(c.faceResolution-1U);
        const auto d=FaceDirection(face,u,v);

        const f64 variation=
            (Fractal(d,13.0,p.seed^0x434f4c4f52ULL)-0.5)*2.0;
        const f64 crater=
            std::clamp(
                -CraterRelief(p,d)/
                std::max(p.craterDepth,1.0e-5),
                0.0,1.0);
        const f64 fresh=
            std::clamp(
                crater*0.7+
                std::max(variation,0.0)*0.18,
                0.0,1.0);

        auto color=
            Lerp(
                p.regolithColorLinear,
                p.freshMaterialColorLinear,
                fresh);
        color=color*
            std::max(
                0.2,
                1.0+variation*p.colorVariation);

        auto& t=result.texels[
            static_cast<std::size_t>(face)*c.faceResolution*c.faceResolution+
            static_cast<std::size_t>(y)*c.faceResolution+x];

        t.albedoLinear={
            static_cast<f32>(std::max(color.x,0.0)),
            static_cast<f32>(std::max(color.y,0.0)),
            static_cast<f32>(std::max(color.z,0.0))};
        t.normal={
            static_cast<f32>(d.x),
            static_cast<f32>(d.y),
            static_cast<f32>(d.z)};
        t.roughness=static_cast<f32>(
            std::clamp(
                p.surfaceRoughness+
                0.06*variation,
                0.0,1.0));
        t.directLightTransmittance=1.0F;
    }

    return result;
}

f64 EvaluateRoughSurfacePhotometry(
    const SmallBodyParameters& p,
    const RoughSurfacePhotometryInput& input) noexcept
{
    const auto n=math::Normalize(input.normal);
    const auto l=math::Normalize(input.lightDirection);
    const auto v=math::Normalize(input.viewDirection);

    const f64 mu0=std::max(math::Dot(n,l),0.0);
    const f64 mu=std::max(math::Dot(n,v),0.0);

    if(mu0<=0.0 || mu<=0.0)
        return 0.0;

    const f64 cosAlpha=
        std::clamp(math::Dot(l,v),-1.0,1.0);
    const f64 alpha=std::acos(cosAlpha);

    // Compact Hapke/Lommel-Seeliger baseline. It preserves strong low-phase
    // opposition behavior without pretending to be a full particulate solver.
    const f64 opposition=
        1.0+
        p.oppositionStrength/
        (1.0+
         std::tan(alpha*0.5)/
         std::max(p.oppositionWidthRadians,1.0e-6));

    const f64 lommelSeeliger=
        mu0/std::max(mu0+mu,1.0e-6);

    const f64 roughnessShadowing=
        std::exp(
            -p.macroscopicRoughnessRadians*
             p.macroscopicRoughnessRadians*
             (1.0-mu0)*(1.0-mu));

    const f64 multipleScatter=
        p.singleScatteringAlbedo*
        mu0*
        0.18;

    return std::max(
        0.0,
        (lommelSeeliger*opposition+multipleScatter)*
        roughnessShadowing);
}
} // namespace orbit::celestial_small_bodies
