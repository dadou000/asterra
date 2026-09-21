#include <orbit/celestial_magnetosphere/Magnetosphere.hpp>

#include <orbit/terrain/TerrainContracts.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace orbit::celestial_magnetosphere
{
namespace
{
[[nodiscard]] math::Double3 SafeNormalize(
    math::Double3 value,
    const math::Double3 fallback) noexcept
{
    return math::LengthSquared(value) > 1.0e-24
        ? math::Normalize(value)
        : fallback;
}

void Validate(
    const MagnetosphereParameters& p,
    const f64 radius,
    const MagnetosphereConfig& c)
{
    const auto finite=[](const f64 v){return std::isfinite(v);};

    if(!finite(radius) || radius<=0.0 ||
       c.ovalSamples<16U ||
       math::LengthSquared(p.dipoleAxis)<=1.0e-24 ||
       math::LengthSquared(p.solarWindDirection)<=1.0e-24 ||
       !finite(p.equatorialFieldTesla) || p.equatorialFieldTesla<0.0 ||
       !finite(p.subsolarStandoffBodyRadii) || p.subsolarStandoffBodyRadii<=1.0 ||
       !finite(p.magnetopauseFlaringAlpha) || p.magnetopauseFlaringAlpha<=0.0 ||
       !finite(p.maximumTailBodyRadii) ||
       p.maximumTailBodyRadii<p.subsolarStandoffBodyRadii ||
       !finite(p.solarWindDynamicPressurePascals) ||
       p.solarWindDynamicPressurePascals<0.0 ||
       !finite(p.interplanetaryFieldBzTesla) ||
       !finite(p.activity) || p.activity<0.0 || p.activity>1.0 ||
       !finite(p.auroralOvalLatitudeDegrees) ||
       p.auroralOvalLatitudeDegrees<0.0 ||
       p.auroralOvalLatitudeDegrees>90.0 ||
       !finite(p.auroralOvalWidthDegrees) ||
       p.auroralOvalWidthDegrees<=0.0 ||
       p.auroralOvalWidthDegrees>45.0 ||
       !finite(p.auroralMinimumAltitudeMeters) ||
       !finite(p.auroralMaximumAltitudeMeters) ||
       p.auroralMinimumAltitudeMeters<0.0 ||
       p.auroralMaximumAltitudeMeters<
           p.auroralMinimumAltitudeMeters ||
       !finite(p.auroralIntensity) ||
       p.auroralIntensity<0.0 ||
       !finite(p.auroralColorLinear.x) ||
       !finite(p.auroralColorLinear.y) ||
       !finite(p.auroralColorLinear.z) ||
       p.auroralColorLinear.x<0.0 ||
       p.auroralColorLinear.y<0.0 ||
       p.auroralColorLinear.z<0.0 ||
       !finite(p.auroralStructure) ||
       p.auroralStructure<0.0 ||
       p.auroralStructure>1.0)
    {
        throw std::invalid_argument(
            "Magnetosphere parameters are invalid.");
    }
}

[[nodiscard]] math::Double3 RingPoint(
    const math::Double3 axis,
    const f64 latitudeRadians,
    const f64 longitudeRadians)
{
    math::Double3 tangent{
        1.0,0.0,0.0};

    if(std::abs(math::Dot(axis,tangent))>0.9)
        tangent={0.0,1.0,0.0};

    const auto u=
        math::Normalize(
            tangent-
            axis*math::Dot(axis,tangent));
    const auto v=
        math::Normalize(
            math::Cross(axis,u));

    const f64 polar=
        std::numbers::pi_v<f64>*0.5-
        latitudeRadians;

    return math::Normalize(
        axis*std::cos(polar)+
        (u*std::cos(longitudeRadians)+
         v*std::sin(longitudeRadians))*
        std::sin(polar));
}
} // namespace

u64 MagnetosphereFingerprint(
    const MagnetosphereParameters& p,
    const f64 referenceRadiusMeters,
    const MagnetosphereConfig& c)
{
    Validate(p,referenceRadiusMeters,c);
    u64 h=0x4d32394d41474e45ULL;
    const auto add=[&](const u64 v)
    {
        h=terrain::StableCombine64(h,v);
    };
    const auto addf=[&](const f64 v)
    {
        add(std::bit_cast<u64>(v));
    };

    for(const f64 v:{
        referenceRadiusMeters,
        p.dipoleAxis.x,p.dipoleAxis.y,p.dipoleAxis.z,
        p.equatorialFieldTesla,
        p.solarWindDirection.x,p.solarWindDirection.y,p.solarWindDirection.z,
        p.subsolarStandoffBodyRadii,
        p.magnetopauseFlaringAlpha,
        p.maximumTailBodyRadii,
        p.solarWindDynamicPressurePascals,
        p.interplanetaryFieldBzTesla,
        p.activity,
        p.auroralOvalLatitudeDegrees,
        p.auroralOvalWidthDegrees,
        p.auroralMinimumAltitudeMeters,
        p.auroralMaximumAltitudeMeters,
        p.auroralIntensity,
        p.auroralColorLinear.x,
        p.auroralColorLinear.y,
        p.auroralColorLinear.z,
        p.auroralStructure})
        addf(v);

    add(p.auroralSeed);
    add(c.ovalSamples);
    return h;
}

math::Double3 EvaluateDipoleFieldTesla(
    const MagnetosphereParameters& p,
    const f64 referenceRadiusMeters,
    const math::Double3 positionBodyMeters) noexcept
{
    const f64 distance=
        math::Length(positionBodyMeters);

    if(distance<=1.0e-12 ||
       referenceRadiusMeters<=0.0)
        return {};

    const auto rhat=
        positionBodyMeters/distance;
    const auto axis=
        SafeNormalize(
            p.dipoleAxis,
            {0.0,0.0,1.0});

    const f64 radialScale=
        std::pow(
            referenceRadiusMeters/distance,
            3.0);

    // B_eq is defined at one reference radius on the magnetic equator.
    return
        (rhat*
             (3.0*
              p.equatorialFieldTesla*
              math::Dot(axis,rhat))-
         axis*
             p.equatorialFieldTesla)*
        radialScale;
}

f64 MagnetopauseRadiusMeters(
    const MagnetosphereParameters& p,
    const f64 referenceRadiusMeters,
    const math::Double3 unitDirectionBody) noexcept
{
    if(referenceRadiusMeters<=0.0)
        return 0.0;

    const auto direction=
        SafeNormalize(
            unitDirectionBody,
            {1.0,0.0,0.0});
    const auto sunward=
        -SafeNormalize(
            p.solarWindDirection,
            {-1.0,0.0,0.0});

    const f64 cosTheta=
        std::clamp(
            math::Dot(direction,sunward),
            -0.999999,
            1.0);

    // Shue-style axisymmetric magnetopause envelope:
    // r = r0 * [2 / (1 + cos(theta))]^alpha.
    const f64 shape=
        std::pow(
            2.0/
            std::max(
                1.0+cosTheta,
                1.0e-6),
            p.magnetopauseFlaringAlpha);

    const f64 radiusBodyRadii=
        std::min(
            p.subsolarStandoffBodyRadii*
                shape,
            p.maximumTailBodyRadii);

    return
        radiusBodyRadii*
        referenceRadiusMeters;
}

f64 AuroralOvalWeight(
    const MagnetosphereParameters& p,
    const math::Double3 unitDirectionBody) noexcept
{
    const auto d=
        SafeNormalize(
            unitDirectionBody,
            {0.0,0.0,1.0});
    const auto axis=
        SafeNormalize(
            p.dipoleAxis,
            {0.0,0.0,1.0});

    const f64 magneticLatitude=
        std::asin(
            std::clamp(
                math::Dot(d,axis),
                -1.0,
                1.0))*
        180.0/
        std::numbers::pi_v<f64>;

    const f64 target=
        std::clamp(
            p.auroralOvalLatitudeDegrees-
            9.0*p.activity,
            0.0,
            90.0);

    const f64 sigma=
        std::max(
            p.auroralOvalWidthDegrees*
            (0.45+0.55*p.activity),
            0.1);

    const f64 delta=
        std::abs(magneticLatitude)-
        target;

    return
        std::exp(
            -0.5*
            (delta/sigma)*
            (delta/sigma));
}

MagnetosphereProduct BuildMagnetosphereProduct(
    const MagnetosphereParameters& p,
    const f64 referenceRadiusMeters,
    const MagnetosphereConfig& c)
{
    const u64 fingerprint=
        MagnetosphereFingerprint(
            p,
            referenceRadiusMeters,
            c);

    const auto axis=
        SafeNormalize(
            p.dipoleAxis,
            {0.0,0.0,1.0});

    const f64 centerLatitude=
        std::clamp(
            p.auroralOvalLatitudeDegrees-
            9.0*p.activity,
            0.0,
            90.0);

    const f64 altitude=
        0.5*
        (p.auroralMinimumAltitudeMeters+
         p.auroralMaximumAltitudeMeters);
    const f64 ringRadius=
        referenceRadiusMeters+
        altitude;

    MagnetosphereProduct result{
        .fingerprint=fingerprint,
        .referenceRadiusMeters=referenceRadiusMeters,
        .subsolarStandoffMeters=
            p.subsolarStandoffBodyRadii*
            referenceRadiusMeters,
        .tailExtentMeters=
            p.maximumTailBodyRadii*
            referenceRadiusMeters,
        .auroralCenterLatitudeDegrees=
            centerLatitude,
        .auroralHalfWidthDegrees=
            p.auroralOvalWidthDegrees*0.5
    };

    result.northAuroralRing.reserve(c.ovalSamples);
    result.southAuroralRing.reserve(c.ovalSamples);

    for(u32 i=0U;i<c.ovalSamples;++i)
    {
        const f64 longitude=
            2.0*
            std::numbers::pi_v<f64>*
            static_cast<f64>(i)/
            static_cast<f64>(c.ovalSamples);

        result.northAuroralRing.push_back(
            RingPoint(
                axis,
                centerLatitude*
                    std::numbers::pi_v<f64>/
                    180.0,
                longitude)*
            ringRadius);

        result.southAuroralRing.push_back(
            RingPoint(
                -axis,
                centerLatitude*
                    std::numbers::pi_v<f64>/
                    180.0,
                longitude)*
            ringRadius);
    }

    return result;
}

AuroraMeshProduct BuildAuroraCurtainMesh(
    const MagnetosphereParameters& p,
    const f64 referenceRadiusMeters,
    const u32 angularSegments)
{
    if(angularSegments<16U)
        throw std::invalid_argument(
            "Aurora curtain mesh requires at least 16 angular segments.");

    Validate(
        p,
        referenceRadiusMeters,
        {.ovalSamples=angularSegments});

    const auto axis=
        SafeNormalize(
            p.dipoleAxis,
            {0.0,0.0,1.0});

    math::Double3 tangent{1.0,0.0,0.0};
    if(std::abs(math::Dot(axis,tangent))>0.9)
        tangent={0.0,1.0,0.0};

    const auto u=
        math::Normalize(
            tangent-axis*math::Dot(axis,tangent));
    const auto v=
        math::Normalize(
            math::Cross(axis,u));

    const f64 centerLatitudeDegrees=
        std::clamp(
            p.auroralOvalLatitudeDegrees-
            9.0*p.activity,
            0.0,
            90.0);
    const f64 latitude=
        centerLatitudeDegrees*
        std::numbers::pi_v<f64>/
        180.0;
    const f64 polar=
        std::numbers::pi_v<f64>*0.5-
        latitude;

    const f64 innerRadius=
        referenceRadiusMeters+
        p.auroralMinimumAltitudeMeters;
    const f64 outerRadius=
        referenceRadiusMeters+
        p.auroralMaximumAltitudeMeters;

    AuroraMeshProduct result;
    result.referenceRadiusMeters=
        referenceRadiusMeters;
    result.angularSegments=
        angularSegments;
    result.fingerprint=
        terrain::StableCombine64(
            MagnetosphereFingerprint(
                p,
                referenceRadiusMeters,
                {.ovalSamples=angularSegments}),
            0x4155524f52414d53ULL);

    result.vertices.reserve(
        static_cast<std::size_t>(
            angularSegments+1U)*
        4U);
    result.indices.reserve(
        static_cast<std::size_t>(
            angularSegments)*
        12U);

    const auto emitHemisphere=
        [&](const f64 hemisphereSign)
        {
            const u32 base=
                static_cast<u32>(
                    result.vertices.size());

            for(u32 i=0U;i<=angularSegments;++i)
            {
                const f64 t=
                    static_cast<f64>(i)/
                    static_cast<f64>(angularSegments);
                const f64 longitude=
                    2.0*
                    std::numbers::pi_v<f64>*
                    t;

                const auto ringDirection=
                    math::Normalize(
                        axis*
                            (hemisphereSign*
                             std::cos(polar))+
                        (u*std::cos(longitude)+
                         v*std::sin(longitude))*
                            std::sin(polar));

                const f64 structurePhase=
                    std::sin(
                        longitude*
                            (3.0+
                             static_cast<f64>(
                                 (p.auroralSeed%5U)))+
                        static_cast<f64>(
                            p.auroralSeed&0xffffU)*
                            0.0017);

                const f64 modulation=
                    std::clamp(
                        1.0+
                        p.auroralStructure*
                            0.45*
                            structurePhase,
                        0.15,
                        1.75);

                const math::Float3 emission{
                    static_cast<f32>(
                        p.auroralColorLinear.x*
                        p.auroralIntensity*
                        modulation),
                    static_cast<f32>(
                        p.auroralColorLinear.y*
                        p.auroralIntensity*
                        modulation),
                    static_cast<f32>(
                        p.auroralColorLinear.z*
                        p.auroralIntensity*
                        modulation)
                };

                const f32 bottomOpacity=
                    static_cast<f32>(
                        std::clamp(
                            0.28+
                            0.42*
                            p.activity*
                            modulation,
                            0.04,
                            0.92));

                const f32 topOpacity=
                    bottomOpacity*
                    0.18F;

                for(const auto [radius,opacity]:
                    std::array<std::pair<f64,f32>,2>{
                        std::pair{innerRadius,bottomOpacity},
                        std::pair{outerRadius,topOpacity}})
                {
                    const auto position=
                        ringDirection*
                        (radius/
                         referenceRadiusMeters);

                    result.vertices.push_back({
                        .positionNormalized={
                            static_cast<f32>(position.x),
                            static_cast<f32>(position.y),
                            static_cast<f32>(position.z)},
                        .emissionLinear=emission,
                        .presentation={opacity,0.0F}
                    });
                }
            }

            for(u32 i=0U;i<angularSegments;++i)
            {
                const u32 i0=base+i*2U;
                const u32 i1=i0+1U;
                const u32 i2=i0+2U;
                const u32 i3=i0+3U;

                result.indices.insert(
                    result.indices.end(),
                    {i0,i2,i1,
                     i1,i2,i3});
            }
        };

    emitHemisphere(1.0);
    emitHemisphere(-1.0);

    return result;
}
} // namespace orbit::celestial_magnetosphere
