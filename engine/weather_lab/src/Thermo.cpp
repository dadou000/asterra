#include <orbit/weather_lab/Thermo.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::weather_lab
{
float SaturationMixingRatio(const float pressure, const float temperature)
{
    const float es = 611.2F * std::exp(
        17.67F * (temperature - 273.15F) / (temperature - 29.65F));
    return 0.622F * es / std::max(pressure - es, 1.0F);
}

namespace
{
struct Profile
{
    std::vector<double> theta;
    std::vector<double> vapor;
    std::vector<double> exner;
    double step = 25.0;

    [[nodiscard]] double Sample(
        const std::vector<double>& table, const double z) const
    {
        const double s = std::max(z, 0.0) / step;
        const auto i = std::min(
            static_cast<std::size_t>(s), table.size() - 2U);
        const double t = std::min(s - static_cast<double>(i), 1.0);
        return table[i] * (1.0 - t) + table[i + 1U] * t;
    }
};

Profile BuildProfile(const SupercellSoundingParams& p, const double top)
{
    using namespace thermo;
    Profile profile;
    const auto count = static_cast<std::size_t>(top / profile.step) + 3U;
    profile.theta.resize(count);
    profile.vapor.assign(count, 0.0);
    profile.exner.resize(count);
    std::vector<double> rh(count, 0.0);
    for (std::size_t i = 0; i < count; ++i)
    {
        const double z = static_cast<double>(i) * profile.step;
        if (z < p.tropopauseHeight)
        {
            const double f = std::pow(z / p.tropopauseHeight, 1.25);
            profile.theta[i] = p.surfaceTheta
                + (p.tropopauseTheta - p.surfaceTheta) * f;
            rh[i] = 1.0 - 0.75 * f;
        }
        else
        {
            profile.theta[i] = p.tropopauseTheta * std::exp(
                (kGravity / (p.tropopauseTemperature * kCp))
                * (z - p.tropopauseHeight));
            rh[i] = 0.25;
        }
    }
    const double piSurface = std::pow(
        static_cast<double>(p.surfacePressure) / kP00, kRd / kCp);
    const double qvSurface = SaturationMixingRatio(
        p.surfacePressure,
        p.surfaceTheta * static_cast<float>(piSurface));
    const double thvSurface = p.surfaceTheta
        * (1.0 + qvSurface * kRepsilon) / (1.0 + qvSurface);
    for (int iteration = 0; iteration < 20; ++iteration)
    {
        auto thv = [&](const std::size_t i)
        {
            return profile.theta[i]
                * (1.0 + kRepsilon * profile.vapor[i])
                / (1.0 + profile.vapor[i]);
        };
        profile.exner[0] = piSurface;
        for (std::size_t i = 1; i < count; ++i)
        {
            const double prevThv = i == 1U ? thvSurface : thv(i - 1U);
            profile.exner[i] = profile.exner[i - 1U]
                - kGravity * profile.step
                    / (kCp * 0.5 * (thv(i) + prevThv));
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            const double pressure =
                kP00 * std::pow(profile.exner[i], kCp / kRd);
            const double temperature = profile.theta[i] * profile.exner[i];
            profile.vapor[i] = std::min(
                rh[i] * SaturationMixingRatio(
                    static_cast<float>(pressure),
                    static_cast<float>(temperature)),
                static_cast<double>(p.boundaryLayerMixingRatio));
        }
    }
    return profile;
}
} // namespace

BaseState BuildSupercellBaseState(
    const SupercellSoundingParams& params,
    const std::uint32_t layers,
    const float layerThickness)
{
    using namespace thermo;
    const double top = static_cast<double>(layers) * layerThickness;
    const Profile profile = BuildProfile(params, top);
    BaseState base;
    const std::size_t n = layers;
    base.centreHeight.resize(n);
    base.theta.resize(n);
    base.vapor.resize(n);
    base.exner.resize(n);
    base.pressure.resize(n);
    base.density.resize(n);
    base.faceDensity.resize(n + 1U);
    base.windU.resize(n);
    base.windV.resize(n);
    auto densityAt = [&](const double z)
    {
        const double exner = profile.Sample(profile.exner, z);
        const double theta = profile.Sample(profile.theta, z);
        const double qv = profile.Sample(profile.vapor, z);
        const double pressure = kP00 * std::pow(exner, kCp / kRd);
        const double temperature = theta * exner;
        return pressure / (kRd * temperature * (1.0 + 0.608 * qv));
    };
    for (std::size_t k = 0; k < n; ++k)
    {
        const double z = (static_cast<double>(k) + 0.5) * layerThickness;
        base.centreHeight[k] = static_cast<float>(z);
        base.theta[k] = static_cast<float>(profile.Sample(profile.theta, z));
        base.vapor[k] = static_cast<float>(profile.Sample(profile.vapor, z));
        base.exner[k] = static_cast<float>(profile.Sample(profile.exner, z));
        base.pressure[k] = static_cast<float>(
            kP00 * std::pow(base.exner[k], kCp / kRd));
        base.density[k] = static_cast<float>(densityAt(z));

        const double u1 = params.windDepthLow;
        const double u2 = params.windDepthHigh;
        const double um1 = params.windSpeedLow;
        const double um2 = params.windSpeedHigh;
        constexpr double kPi = 3.14159265358979323846;
        if (z <= u1)
        {
            base.windU[k] = static_cast<float>(
                um1 - um1 * std::cos(0.5 * kPi * z / u1));
            base.windV[k] = static_cast<float>(
                um1 * std::sin(0.5 * kPi * z / u1));
        }
        else
        {
            base.windU[k] = static_cast<float>(
                z <= u2 ? um1 + (z - u1) * (um2 - um1) / (u2 - u1) : um2);
            base.windV[k] = static_cast<float>(um1);
        }
    }
    for (std::size_t k = 0; k <= n; ++k)
    {
        base.faceDensity[k] = static_cast<float>(
            densityAt(static_cast<double>(k) * layerThickness));
    }
    return base;
}
} // namespace orbit::weather_lab
