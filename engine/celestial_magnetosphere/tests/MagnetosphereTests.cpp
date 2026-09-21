#include <orbit/celestial_magnetosphere/Magnetosphere.hpp>

#include <algorithm>
#include <cmath>

int main()
{
    using namespace orbit;
    using namespace orbit::celestial_magnetosphere;

    MagnetosphereParameters p{};
    constexpr f64 radius=6.371e6;

    const auto product=
        BuildMagnetosphereProduct(
            p,
            radius,
            {.ovalSamples=64U});

    if(product.fingerprint==0U ||
       product.northAuroralRing.size()!=64U ||
       product.southAuroralRing.size()!=64U)
        return 1;

    const f64 nose=
        MagnetopauseRadiusMeters(
            p,
            radius,
            {1.0,0.0,0.0});
    const f64 tail=
        MagnetopauseRadiusMeters(
            p,
            radius,
            {-1.0,0.0,0.0});

    if(!(tail>nose))
        return 2;

    const auto equatorialField=
        EvaluateDipoleFieldTesla(
            p,
            radius,
            {radius,0.0,0.0});

    if(std::abs(
            math::Length(equatorialField)-
            p.equatorialFieldTesla)>
        1.0e-10)
        return 3;

    const f64 oval=
        AuroralOvalWeight(
            p,
            {
                std::cos(
                    67.0*
                    3.14159265358979323846/
                    180.0),
                0.0,
                std::sin(
                    67.0*
                    3.14159265358979323846/
                    180.0)
            });

    const f64 equator=
        AuroralOvalWeight(
            p,
            {1.0,0.0,0.0});

    if(!(oval>equator))
        return 4;

    const auto curtain=
        BuildAuroraCurtainMesh(
            p,
            radius,
            64U);

    if(curtain.fingerprint==0U ||
       curtain.vertices.size()!=
           4U*65U ||
       curtain.indices.size()!=
           12U*64U)
        return 5;

    f32 maximumEmission=0.0F;
    for(const auto& vertex:curtain.vertices)
    {
        maximumEmission=
            std::max(
                maximumEmission,
                vertex.emissionLinear.y);
    }

    if(!(maximumEmission>1.0F))
        return 6;

    auto changed=p;
    changed.activity=0.9;

    const auto activeProduct=
        BuildMagnetosphereProduct(
            changed,
            radius,
            {.ovalSamples=64U});

    if(MagnetosphereFingerprint(
           changed,radius,{.ovalSamples=64U})==
           product.fingerprint ||
       !(activeProduct.
             auroralCenterLatitudeDegrees<
         product.
             auroralCenterLatitudeDegrees))
        return 7;

    return 0;
}
