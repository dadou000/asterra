#include <orbit/celestial_rings/RingSystem.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace orbit::celestial_rings
{
namespace
{
[[nodiscard]] math::Double3 SafeNormal(
    const math::Double3 v)
{
    if (math::LengthSquared(v) <= 1.0e-20)
        throw std::invalid_argument("Ring plane normal is invalid.");
    return math::Normalize(v);
}

[[nodiscard]] std::pair<math::Double3, math::Double3>
RingBasis(const math::Double3 normal)
{
    const auto n=SafeNormal(normal);
    const math::Double3 ref=
        std::abs(n.y)<0.9 ? math::Double3{0,1,0}:math::Double3{1,0,0};
    const auto a=math::Normalize(math::Cross(ref,n));
    const auto b=math::Normalize(math::Cross(n,a));
    return {a,b};
}

[[nodiscard]] const RingBand* BandAtRadius(
    const RingSystem& system,
    const f64 radius)
{
    for(const auto& band:system.bands)
        if(radius>=band.innerRadiusMeters &&
           radius<=band.outerRadiusMeters)
            return &band;
    return nullptr;
}
} // namespace

void ValidateRingSystem(const RingSystem& system)
{
    static_cast<void>(SafeNormal(system.planeNormalBody));
    f64 previousOuter=0.0;
    for(const auto& b:system.bands)
    {
        if(!std::isfinite(b.innerRadiusMeters) ||
           !std::isfinite(b.outerRadiusMeters) ||
           b.innerRadiusMeters<0.0 ||
           b.outerRadiusMeters<=b.innerRadiusMeters ||
           !std::isfinite(b.normalOpticalDepth) ||
           b.normalOpticalDepth<0.0 ||
           b.singleScatteringAlbedo<0.0 ||
           b.singleScatteringAlbedo>1.0 ||
           b.anisotropy<=-1.0 || b.anisotropy>=1.0 ||
           b.colorLinear.x<0.0 || b.colorLinear.y<0.0 || b.colorLinear.z<0.0 ||
           b.thicknessMeters<0.0)
            throw std::invalid_argument("Ring band parameters are invalid.");

        if(b.innerRadiusMeters<previousOuter)
            throw std::invalid_argument("Ring bands must not overlap.");
        previousOuter=b.outerRadiusMeters;
    }
}

u64 RingSystemFingerprint(const RingSystem& system)
{
    ValidateRingSystem(system);
    u64 h=0x4d323552494e4753ULL;
    const auto add=[&](u64 v){h=terrain::StableCombine64(h,v);};
    const auto addf=[&](f64 v){add(std::bit_cast<u64>(v));};
    const auto n=SafeNormal(system.planeNormalBody);
    addf(n.x); addf(n.y); addf(n.z);
    add(system.castRingShadows?1U:0U);
    add(system.receiveBodyShadow?1U:0U);
    for(const auto& b:system.bands)
    {
        add(b.semanticIdHigh); add(b.semanticIdLow);
        addf(b.innerRadiusMeters); addf(b.outerRadiusMeters);
        addf(b.normalOpticalDepth); addf(b.singleScatteringAlbedo);
        addf(b.anisotropy); addf(b.colorLinear.x); addf(b.colorLinear.y);
        addf(b.colorLinear.z); addf(b.thicknessMeters);
    }
    return h;
}

f64 HenyeyGreensteinPhase(const f64 cosineTheta,const f64 g)
{
    if(g<=-1.0 || g>=1.0)
        throw std::invalid_argument("Ring phase anisotropy must be in (-1,1).");
    const f64 c=std::clamp(cosineTheta,-1.0,1.0);
    const f64 d=1.0+g*g-2.0*g*c;
    return (1.0-g*g)/(4.0*std::numbers::pi_v<f64>*std::pow(d,1.5));
}

f64 RingTransmission(const f64 tau,const f64 cosineNormal)
{
    if(tau<0.0)
        throw std::invalid_argument("Ring optical depth must be nonnegative.");
    const f64 mu=std::max(std::abs(cosineNormal),1.0e-4);
    return std::exp(-tau/mu);
}

f64 RingShadowTransmittanceAtSurface(
    const RingSystem& system,
    const f64 bodyRadius,
    math::Double3 surfaceDir,
    math::Double3 lightDir)
{
    ValidateRingSystem(system);
    if(!system.castRingShadows || system.bands.empty())
        return 1.0;
    if(bodyRadius<=0.0)
        throw std::invalid_argument("Body radius must be positive.");

    const auto n=SafeNormal(system.planeNormalBody);
    surfaceDir=SafeNormal(surfaceDir);
    lightDir=SafeNormal(lightDir);
    const auto origin=surfaceDir*bodyRadius;
    const f64 denom=math::Dot(lightDir,n);
    if(std::abs(denom)<1.0e-9)
        return 1.0;
    const f64 t=-math::Dot(origin,n)/denom;
    if(t<=0.0)
        return 1.0;
    const auto p=origin+lightDir*t;
    const auto planar=p-n*math::Dot(p,n);
    const f64 radius=math::Length(planar);
    const auto* band=BandAtRadius(system,radius);
    if(band==nullptr)
        return 1.0;
    return RingTransmission(band->normalOpticalDepth,denom);
}

f64 BodyShadowTransmittanceAtRingPoint(
    const RingSystem& system,
    const f64 bodyRadius,
    math::Double3 point,
    math::Double3 lightDir)
{
    ValidateRingSystem(system);
    if(!system.receiveBodyShadow)
        return 1.0;
    if(bodyRadius<=0.0)
        throw std::invalid_argument("Body radius must be positive.");
    lightDir=SafeNormal(lightDir);

    const f64 b=2.0*math::Dot(point,lightDir);
    const f64 c=math::Dot(point,point)-bodyRadius*bodyRadius;
    const f64 disc=b*b-4.0*c;
    if(disc<0.0)
        return 1.0;
    const f64 root=std::sqrt(disc);
    const f64 t0=(-b-root)*0.5;
    const f64 t1=(-b+root)*0.5;
    return (t0>1.0e-9 || t1>1.0e-9) ? 0.0 : 1.0;
}

RingMeshProduct BuildRingMesh(
    const RingSystem& system,
    const f64 referenceRadius,
    const u32 segments)
{
    ValidateRingSystem(system);
    if(referenceRadius<=0.0 || segments<8U)
        throw std::invalid_argument("Ring mesh config is invalid.");

    RingMeshProduct result;
    result.referenceRadiusMeters=referenceRadius;
    result.angularSegments=segments;
    result.fingerprint=
        terrain::StableCombine64(
            terrain::StableCombine64(
                RingSystemFingerprint(system),
                std::bit_cast<u64>(referenceRadius)),
            segments);
    const auto [a,b]=RingBasis(system.planeNormalBody);

    for(const auto& band:system.bands)
    {
        const u32 base=static_cast<u32>(result.vertices.size());
        for(u32 i=0;i<=segments;++i)
        {
            const f64 theta=2.0*std::numbers::pi_v<f64>*
                static_cast<f64>(i)/static_cast<f64>(segments);
            const auto dir=a*std::cos(theta)+b*std::sin(theta);
            for(const f64 radius:{band.innerRadiusMeters,band.outerRadiusMeters})
            {
                const auto p=dir*(radius/referenceRadius);
                result.vertices.push_back({
                    .positionNormalized={
                        static_cast<f32>(p.x),
                        static_cast<f32>(p.y),
                        static_cast<f32>(p.z)},
                    .colorLinear={
                        static_cast<f32>(band.colorLinear.x),
                        static_cast<f32>(band.colorLinear.y),
                        static_cast<f32>(band.colorLinear.z)},
                    .optical={
                        static_cast<f32>(band.normalOpticalDepth),
                        static_cast<f32>(band.singleScatteringAlbedo),
                        static_cast<f32>(band.anisotropy)}
                });
            }
        }
        for(u32 i=0;i<segments;++i)
        {
            const u32 i0=base+i*2U;
            const u32 i1=i0+1U;
            const u32 i2=i0+2U;
            const u32 i3=i0+3U;
            result.indices.insert(result.indices.end(),{i0,i2,i1,i1,i2,i3});
        }
    }
    return result;
}

FarRingProfile BuildFarRingProfile(
    const RingSystem& system,
    const f64 referenceRadius,
    const u32 samples)
{
    ValidateRingSystem(system);
    if(referenceRadius<=0.0 || samples<8U)
        throw std::invalid_argument("Far ring profile config is invalid.");

    FarRingProfile result;
    result.referenceRadiusMeters=referenceRadius;
    result.radialSamples=samples;
    result.fingerprint=
        terrain::StableCombine64(
            terrain::StableCombine64(
                RingSystemFingerprint(system),
                std::bit_cast<u64>(referenceRadius)),
            samples);
    if(system.bands.empty())
        return result;

    result.innerRadiusMeters=system.bands.front().innerRadiusMeters;
    result.outerRadiusMeters=system.bands.back().outerRadiusMeters;
    result.samples.reserve(samples);

    for(u32 i=0;i<samples;++i)
    {
        const f64 t=(static_cast<f64>(i)+0.5)/samples;
        const f64 r=result.innerRadiusMeters+
            (result.outerRadiusMeters-result.innerRadiusMeters)*t;
        FarRingSample s;
        s.radiusNormalized=static_cast<f32>(r/referenceRadius);
        if(const auto* band=BandAtRadius(system,r);band!=nullptr)
        {
            s.opticalDepth=static_cast<f32>(band->normalOpticalDepth);
            s.colorLinear={
                static_cast<f32>(band->colorLinear.x),
                static_cast<f32>(band->colorLinear.y),
                static_cast<f32>(band->colorLinear.z)};
            s.singleScatteringAlbedo=static_cast<f32>(band->singleScatteringAlbedo);
            s.anisotropy=static_cast<f32>(band->anisotropy);
        }
        result.samples.push_back(s);
    }
    return result;
}
} // namespace orbit::celestial_rings
