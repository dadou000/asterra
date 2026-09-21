#include <orbit/celestial_small_bodies/SmallBodyAppearance.hpp>

#include <cmath>

int main()
{
    using orbit::f64;
    using namespace orbit::celestial_small_bodies;

    SmallBodyParameters p{};
    SmallBodyAppearanceConfig c{.faceResolution=17U};

    const auto shapeA=BuildSmallBodyShape(p,c);
    const auto shapeB=BuildSmallBodyShape(p,c);
    const auto appearanceA=BuildSmallBodyAppearance(p,c);
    const auto appearanceB=BuildSmallBodyAppearance(p,c);

    if(shapeA.fingerprint!=shapeB.fingerprint ||
       appearanceA.fingerprint!=appearanceB.fingerprint)
        return 1;

    if(shapeA.radiusScale.size()!=6U*17U*17U ||
       appearanceA.texels.size()!=6U*17U*17U)
        return 2;

    if(!(shapeA.maximumRadiusScale>shapeA.minimumRadiusScale))
        return 3;

    auto elongated=p;
    elongated.axisScale={1.8,0.7,0.55};

    const f64 longAxis=
        EvaluateRadiusScale(elongated,{1.0,0.0,0.0});
    const f64 shortAxis=
        EvaluateRadiusScale(elongated,{0.0,0.0,1.0});

    if(!(longAxis>shortAxis))
        return 4;

    const RoughSurfacePhotometryInput opposition{
        .normal={0.0,0.0,1.0},
        .lightDirection={0.0,0.0,1.0},
        .viewDirection={0.0,0.0,1.0}
    };

    const RoughSurfacePhotometryInput quarter{
        .normal={0.0,0.0,1.0},
        .lightDirection={0.70710678,0.0,0.70710678},
        .viewDirection={-0.70710678,0.0,0.70710678}
    };

    if(!(EvaluateRoughSurfacePhotometry(p,opposition)>
         EvaluateRoughSurfacePhotometry(p,quarter)))
        return 5;

    auto changed=p;
    changed.craterDensity=0.9;
    if(SmallBodyAppearanceFingerprint(changed,c)==
       SmallBodyAppearanceFingerprint(p,c))
        return 6;

    return 0;
}
