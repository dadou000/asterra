#include <orbit/storm_electrification/WarmRainAdapter.hpp>

#include <algorithm>
#include <stdexcept>

namespace orbit::storm_electrification
{
StormFields StormFieldsFromWarmRain(const WarmRainInputs& in, const IceProxyParams& p)
{
    const std::size_t n = in.potentialTemperatureK.size();
    if (in.exner.size() != n || in.airDensity.size() != n || in.updraft.size() != n || in.cloudWater.size() != n ||
        in.rainWater.size() != n)
    {
        throw std::invalid_argument("WarmRainInputs: every array must have one value per cell");
    }
    StormFields f;
    f.temperatureK.resize(n);
    f.airDensity = in.airDensity;
    f.updraft = in.updraft;
    f.graupel.assign(n, 0.0F);
    f.ice.assign(n, 0.0F);
    f.liquid.assign(n, 0.0F);
    for (std::size_t c = 0; c < n; ++c)
    {
        const double temperature = static_cast<double>(in.potentialTemperatureK[c]) * in.exner[c];
        f.temperatureK[c] = static_cast<float>(temperature);
        const double qc = std::max(0.0F, in.cloudWater[c]);
        const double qr = std::max(0.0F, in.rainWater[c]);
        if (temperature < p.graupelBelowK)
        {
            f.graupel[c] = static_cast<float>(qr * p.graupelFraction);
            f.ice[c] = static_cast<float>(qr * (1.0 - p.graupelFraction));
        }
        if (temperature < p.glaciationK)
        {
            f.ice[c] += static_cast<float>(qc);
        }
        else if (temperature < p.freezingK)
        {
            f.liquid[c] = static_cast<float>(qc);
        }
    }
    return f;
}
} // namespace orbit::storm_electrification
