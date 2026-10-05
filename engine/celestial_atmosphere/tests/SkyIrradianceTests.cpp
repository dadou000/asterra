#include <orbit/celestial_atmosphere/SkyIrradiance.hpp>

#include <cmath>
#include <numbers>

using namespace orbit;
using namespace orbit::celestial_atmosphere;

namespace
{
constexpr f64 kPi = std::numbers::pi_v<f64>;

[[nodiscard]] bool Near(const f64 a, const f64 b, const f64 tolerance)
{
    return std::abs(a - b) <= tolerance;
}

// A radiance table with the sky-view layout, filled by `radiance(direction)`.
template <typename Fn>
AtmosphereLut2D MakeLut(const u32 width, const u32 height, Fn&& radiance)
{
    AtmosphereLut2D lut;
    lut.width = width;
    lut.height = height;
    lut.texels.resize(static_cast<std::size_t>(width) * height);
    for (u32 y = 0U; y < height; ++y)
    {
        const f64 theta = kPi * (static_cast<f64>(y) + 0.5) / height;
        for (u32 x = 0U; x < width; ++x)
        {
            const f64 phi = 2.0 * kPi * (static_cast<f64>(x) + 0.5) / width;
            const math::Double3 d{
                std::sin(theta) * std::cos(phi),
                std::sin(theta) * std::sin(phi),
                std::cos(theta)};
            const f64 value = radiance(d);
            lut.texels[static_cast<std::size_t>(y) * width + x] = {
                static_cast<f32>(value), static_cast<f32>(value * 0.5),
                static_cast<f32>(value * 0.25), 1.0F};
        }
    }
    return lut;
}
} // namespace

int main()
{
    // Uniform radiance L gives irradiance pi * L for every normal.
    {
        const auto sh = ProjectSkyViewToSphericalHarmonics(
            MakeLut(64U, 32U, [](const math::Double3&) { return 1.0; }));
        for (const math::Double3 n :
             {math::Double3{0, 0, 1}, math::Double3{0, 0, -1},
              math::Double3{1, 0, 0}, math::Double3{0.3, -0.5, 0.8}})
        {
            const auto e = EvaluateSkyIrradiance(sh, n);
            if (!Near(e.x, kPi, 0.02) ||
                !Near(e.y, kPi * 0.5, 0.02) ||
                !Near(e.z, kPi * 0.25, 0.02))
                return 1;
        }
    }

    // Radiance only from the upper hemisphere: the up normal collects close to
    // pi * L, the down normal close to nothing, and the up normal collects more.
    {
        const auto sh = ProjectSkyViewToSphericalHarmonics(MakeLut(
            64U, 32U, [](const math::Double3& d) { return d.z > 0.0 ? 1.0 : 0.0; }));
        const auto up = EvaluateSkyIrradiance(sh, {0, 0, 1});
        const auto down = EvaluateSkyIrradiance(sh, {0, 0, -1});
        const auto side = EvaluateSkyIrradiance(sh, {1, 0, 0});
        if (!Near(up.x, kPi, 0.35) || down.x > 0.35 || !(up.x > side.x) ||
            !(side.x > down.x))
            return 2;
    }

    // Radiance concentrated toward +X lights a +X normal more than a -X normal.
    {
        const auto sh = ProjectSkyViewToSphericalHarmonics(MakeLut(
            64U, 32U, [](const math::Double3& d) { return d.x > 0.8 ? 4.0 : 0.1; }));
        if (!(EvaluateSkyIrradiance(sh, {1, 0, 0}).x >
              EvaluateSkyIrradiance(sh, {-1, 0, 0}).x * 2.0))
            return 3;
    }

    // Sky-frame sun: overhead sun is on the zenith; a horizon sun is horizontal.
    {
        const auto overhead = SkyFrameSunDirection({0, 1, 0}, {0, 1, 0});
        if (!Near(overhead.z, 1.0, 1e-9) || !Near(overhead.x, 0.0, 1e-9))
            return 4;
        const auto horizon = SkyFrameSunDirection({0, 1, 0}, {1, 0, 0});
        if (!Near(horizon.z, 0.0, 1e-9) || !Near(horizon.x, 1.0, 1e-9))
            return 5;
        const auto night = SkyFrameSunDirection({0, 1, 0}, {0, -1, 0});
        if (!Near(night.z, -1.0, 1e-9))
            return 6;
        // The body-fixed sun direction no longer leaks in: an observer elsewhere
        // on the planet with the same local geometry gets the same sky frame.
        const auto elsewhere = SkyFrameSunDirection({1, 0, 0}, {1, 0, 0});
        if (!Near(elsewhere.z, overhead.z, 1e-9))
            return 7;
        // Quantised: tiny motion over the surface keeps the same direction.
        const auto sun = math::Normalize(math::Double3{0.6, 0.8, 0.0});
        const auto a = SkyFrameSunDirection({0, 1, 0}, sun);
        const auto b = SkyFrameSunDirection({0, 1, 1e-5}, sun);
        if (a.z != b.z)
            return 8;
    }

    // The basis is orthonormal and round-trips body vectors.
    {
        const auto up = math::Normalize(math::Double3{0.3, 0.8, -0.2});
        const auto sun = math::Normalize(math::Double3{-0.6, 0.3, 0.7});
        const auto basis = MakeSkyFrameBasis(up, sun);
        if (!Near(math::Length(basis.x), 1.0, 1e-9) ||
            !Near(math::Dot(basis.x, basis.y), 0.0, 1e-9) ||
            !Near(math::Dot(basis.x, basis.z), 0.0, 1e-9) ||
            !Near(math::Dot(basis.y, basis.z), 0.0, 1e-9))
            return 9;
        const math::Double3 v{0.2, -0.4, 0.9};
        const auto back = basis.ToBody(basis.FromBody(v));
        if (!Near(back.x, v.x, 1e-9) || !Near(back.y, v.y, 1e-9) ||
            !Near(back.z, v.z, 1e-9))
            return 10;
        // The sun lies in the +X/+Z plane of its own sky frame.
        const auto sunSky = basis.FromBody(sun);
        if (!Near(sunSky.y, 0.0, 1e-9) || !(sunSky.x >= 0.0))
            return 11;
    }

    // Real atmosphere: an overhead sun gives a brighter, bluer up-facing sky than
    // a sun below the horizon.
    {
        AtmosphereLutConfig config{
            .transmittanceWidth = 24U, .transmittanceHeight = 8U,
            .multiScatteringWidth = 12U, .multiScatteringHeight = 6U,
            .skyViewWidth = 32U, .skyViewHeight = 16U,
            .opticalDepthSteps = 24U, .multiDirectionSamples = 16U,
            .skyViewSteps = 16U};
        const AtmosphereParameters earth;
        const auto luts = BuildStaticLuts(earth, config);

        const auto skyFor = [&](const math::Double3& sunSkyFrame) {
            return BuildSkyView(
                earth, luts,
                {.observerRadiusMeters = earth.bottomRadiusMeters + 2.0,
                 .sunDirectionBody = sunSkyFrame,
                 .incidentIrradianceWattsPerSquareMeter = {1361.0, 1361.0, 1361.0}},
                config);
        };

        const auto day = ProjectSkyViewToSphericalHarmonics(
            skyFor({0.0, 0.0, 1.0}).skyView);
        const auto night = ProjectSkyViewToSphericalHarmonics(
            skyFor({0.0, 0.0, -1.0}).skyView);

        const auto dayUp = EvaluateSkyIrradiance(day, {0, 0, 1});
        const auto nightUp = EvaluateSkyIrradiance(night, {0, 0, 1});
        if (!(dayUp.x > 0.0F) || !(dayUp.z > dayUp.x) ||
            !(dayUp.x > nightUp.x * 10.0F))
            return 12;
    }

    return 0;
}
