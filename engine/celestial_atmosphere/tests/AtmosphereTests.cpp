#include <orbit/celestial_atmosphere/Atmosphere.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

int main()
{
    using namespace orbit::celestial_atmosphere;

    AtmosphereLutConfig config{
        .transmittanceWidth = 24U,
        .transmittanceHeight = 8U,
        .multiScatteringWidth = 12U,
        .multiScatteringHeight = 6U,
        .skyViewWidth = 24U,
        .skyViewHeight = 12U,
        .opticalDepthSteps = 24U,
        .multiDirectionSamples = 16U,
        .skyViewSteps = 16U
    };

    AtmosphereParameters earth;

    // The GPU atmosphere sampling schedule must vary continuously as the
    // closest approach moves through the ray interval. The original
    // 2.5%-threshold mapping jumped by ~0.1 of an orbital ray length,
    // creating concentric dawn/terminator rings in neighboring pixels.
    const auto marchBoundary = [](const double u, const double c)
    {
        const double left = std::sqrt(c);
        const double right = std::sqrt(1.0 - c);
        const double pivot = left / std::max(left + right, 1.0e-6);
        if (u <= pivot)
        {
            const double q = std::clamp(
                (pivot - u) / std::max(pivot, 1.0e-6), 0.0, 1.0);
            return c * (1.0 - q * q * q);
        }
        const double q = std::clamp(
            (u - pivot) / std::max(1.0 - pivot, 1.0e-6), 0.0, 1.0);
        return c + (1.0 - c) * q * q * q;
    };
    for (const double c : {0.0, 0.001, 0.025, 0.25, 0.5,
                           0.75, 0.975, 0.999, 1.0})
    {
        double previous = marchBoundary(0.0, c);
        if (std::abs(previous) > 1.0e-12)
            return 9;
        for (int k = 1; k <= 40; ++k)
        {
            const double next = marchBoundary(double(k) / 40.0, c);
            if (!(next >= previous && next <= 1.0 + 1.0e-12))
                return 10;
            previous = next;
        }
        if (std::abs(previous - 1.0) > 1.0e-12)
            return 11;
    }
    // The values immediately either side of the old arbitrary cutoff
    // must now differ only by the movement of the physical ray.
    for (const double c : {0.025, 0.975})
    {
        for (int k = 0; k <= 40; ++k)
        {
            const double u = double(k) / 40.0;
            const double a = marchBoundary(u, c - 1.0e-6);
            const double b = marchBoundary(u, c + 1.0e-6);
            if (std::abs(b - a) > 1.0e-3)
                return 12;
        }
    }


    // Exact planet-shadow clipping used by the view-ray atmosphere march:
    // the night-side shadow is a convex cylinder intersected with a
    // half-space. Test both the shadow interval and its smooth coverage.
    const auto interval = [](
        const std::array<double, 3>& origin,
        const std::array<double, 3>& direction,
        const std::array<double, 3>& sun,
        const std::array<double, 3>& axes,
        const double t0, const double t1)
    {
        using V = std::array<double, 3>;
        const auto dot = [](const V& a, const V& b)
        { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
        const auto scale = [](const V& a, const double f) -> V
        { return {a[0]*f,a[1]*f,a[2]*f}; };
        const auto sub = [](const V& a, const V& b) -> V
        { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; };
        const auto divide = [](const V& a, const V& b) -> V
        { return {a[0]/b[0],a[1]/b[1],a[2]/b[2]}; };
        const V q0 = divide({origin[0]+direction[0]*t0,
            origin[1]+direction[1]*t0,
            origin[2]+direction[2]*t0}, axes);
        const V qv = divide(scale(direction, t1-t0), axes);
        const V unscaled = divide(sun, axes);
        const V star = scale(unscaled, 1.0/std::sqrt(dot(unscaled,unscaled)));
        const V p0 = sub(q0, scale(star,dot(q0,star)));
        const V pv = sub(qv, scale(star,dot(qv,star)));
        const double a = dot(pv,pv);
        const double b = dot(p0,pv);
        const double c = dot(p0,p0)-1.0;
        double lower = 0.0, upper = 1.0;
        if (a <= 1.0e-16)
        {
            if (c > 0.0) return std::array<double,2>{t1,t1};
        }
        else
        {
            const double disc = b*b-a*c;
            if (disc < 0.0) return std::array<double,2>{t1,t1};
            const double root = std::sqrt(std::max(disc,0.0));
            lower = std::max(lower,(-b-root)/a);
            upper = std::min(upper,(-b+root)/a);
        }
        const double facing = dot(q0,star), travel = dot(qv,star);
        if (travel > 1.0e-9) upper = std::min(upper,-facing/travel);
        else if (travel < -1.0e-9) lower = std::max(lower,-facing/travel);
        else if (facing >= 0.0) return std::array<double,2>{t1,t1};
        if (lower >= upper) return std::array<double,2>{t1,t1};
        return std::array<double,2>{
            t0+lower*(t1-t0),t0+upper*(t1-t0)};
    };
    const std::array<double,3> star{1.0,0.0,0.0};
    const std::array<double,3> yRay{0.0,1.0,0.0};
    const std::array<double,3> unitAxes{1.0,1.0,1.0};
    const auto shadow = interval({-2.0,-2.0,0.6}, yRay,
        star,unitAxes,0.0,4.0);
    if (std::abs(shadow[0]-1.2)>1.0e-12 ||
        std::abs(shadow[1]-2.8)>1.0e-12) return 13;
    const auto sunward = interval({2.0,-2.0,0.6},yRay,
        star,unitAxes,0.0,4.0);
    if (sunward[0]!=4.0 || sunward[1]!=4.0) return 14;
    const auto miss = interval({-2.0,-2.0,2.0},yRay,
        star,unitAxes,0.0,4.0);
    if (miss[0]!=4.0 || miss[1]!=4.0) return 15;
    const auto oblate = interval({-3.0,-2.0,0.6}, yRay,
        star,{2.0,1.0,1.0},0.0,4.0);
    if (std::abs(oblate[0]-1.2)>1.0e-12 ||
        std::abs(oblate[1]-2.8)>1.0e-12) return 16;
    const auto litLength = [](const std::array<double,2>& cut,
        const double from, const double to)
    {
        const double before = std::max(0.0,std::min(to,cut[0])-from);
        const double after = std::max(0.0,to-std::max(from,cut[1]));
        return before+after;
    };
    if (std::abs(litLength(shadow,1.0,2.0)-0.2)>1.0e-12 ||
        std::abs(litLength(shadow,2.0,3.0)-0.2)>1.0e-12 ||
        std::abs(litLength(shadow,0.0,4.0)-2.4)>1.0e-12)
        return 17;
    const auto nearby = interval({-2.0,-2.0,0.600001},yRay,
        star,unitAxes,0.0,4.0);
    if (std::abs(litLength(shadow,1.0,2.0) -
        litLength(nearby,1.0,2.0))>1.0e-4)
        return 18;

    const auto seaLevel =
        SampleAtmosphere(
            earth,
            earth.bottomRadiusMeters);

    const auto high =
        SampleAtmosphere(
            earth,
            earth.bottomRadiusMeters +
                50000.0);

    if (!(seaLevel.rayleighDensity >
            high.rayleighDensity) ||
        !(seaLevel.mieDensity >
            high.mieDensity))
    {
        return 1;
    }

    const auto a =
        BuildStaticLuts(
            earth,
            config);
    const auto b =
        BuildStaticLuts(
            earth,
            config);

    if (a.fingerprint !=
            b.fingerprint ||
        a.transmittance.texels.size() !=
            24U * 8U ||
        a.multiScattering.texels.size() !=
            12U * 6U)
    {
        return 2;
    }

    bool hasTransmission = false;
    bool hasExtinction = false;

    for (const auto value :
         a.transmittance.texels)
    {
        hasTransmission |=
            value.x > 0.5F;
        hasExtinction |=
            value.x < 0.5F;
    }

    if (!hasTransmission ||
        !hasExtinction)
    {
        return 3;
    }

    // Sunlight at the ground reddens toward the horizon: more extinction in
    // blue than red, and less light overall than at the zenith.
    const auto zenithSun =
        SunTransmittanceAt(earth, a, earth.bottomRadiusMeters, 1.0);
    const auto lowSun =
        SunTransmittanceAt(earth, a, earth.bottomRadiusMeters, 0.08);

    if (!(zenithSun.x > lowSun.x) ||
        !(lowSun.x > lowSun.z) ||
        !(zenithSun.z > lowSun.z) ||
        !(zenithSun.x <= 1.0))
    {
        return 5;
    }

    const auto sky =
        BuildSkyView(
            earth,
            a,
            {
                .observerRadiusMeters =
                    earth.bottomRadiusMeters +
                    2.0,
                .sunDirectionBody = {
                    0.0, 0.0, 1.0},
                .incidentIrradianceWattsPerSquareMeter = {
                    1361.0, 1361.0, 1361.0}
            },
            config);

    if (sky.skyView.texels.size() !=
            24U * 12U ||
        sky.fingerprint == 0U)
    {
        return 4;
    }

    bool hasSkyRadiance = false;

    for (const auto value :
         sky.skyView.texels)
    {
        if (value.x > 0.0F ||
            value.y > 0.0F ||
            value.z > 0.0F)
        {
            hasSkyRadiance = true;
            break;
        }
    }

    if (!hasSkyRadiance)
    {
        return 5;
    }

    const auto orbitalSky =
        BuildSkyView(
            earth,
            a,
            {
                .observerRadiusMeters =
                    earth.topRadiusMeters +
                    2.0e6,
                .sunDirectionBody = {
                    0.0, 0.0, 1.0},
                .incidentIrradianceWattsPerSquareMeter = {
                    1361.0, 1361.0, 1361.0}
            },
            config);

    bool hasOrbitalLimb = false;

    for (const auto value :
         orbitalSky.skyView.texels)
    {
        if (value.x > 0.0F ||
            value.y > 0.0F ||
            value.z > 0.0F)
        {
            hasOrbitalLimb = true;
            break;
        }
    }

    if (!hasOrbitalLimb ||
        orbitalSky.fingerprint ==
            sky.fingerprint ||
        a.fingerprint !=
            b.fingerprint)
    {
        return 6;
    }

    const auto darkerSkyFingerprint =
        AtmosphereSkyFingerprint(
            earth,
            a.fingerprint,
            {
                .observerRadiusMeters =
                    earth.bottomRadiusMeters +
                    2.0,
                .sunDirectionBody = {
                    0.0, 0.0, 1.0},
                .incidentIrradianceWattsPerSquareMeter = {
                    680.5, 680.5, 680.5}
            },
            config);

    if (darkerSkyFingerprint ==
        sky.fingerprint)
    {
        return 7;
    }

    const auto revised =
        AtmosphereFingerprint(
            AtmosphereParameters{
                .bottomRadiusMeters =
                    earth.bottomRadiusMeters,
                .topRadiusMeters =
                    earth.topRadiusMeters +
                    1000.0
            },
            config);

    if (revised == a.fingerprint)
    {
        return 8;
    }

    return 0;
}
