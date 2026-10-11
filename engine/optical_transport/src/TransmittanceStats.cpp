#include "orbit/optical_transport/TransmittanceStats.hpp"

#include <algorithm>
#include <cmath>

namespace orbit::optical_transport
{

namespace
{
constexpr double kFloor = 1.0e-12;
}

double TransmittanceStats::EffectiveDepth(double scale) const
{
    return -std::log(std::max(Transmittance(scale), kFloor));
}

double TransmittanceStats::Transmittance(double scale) const
{
    if (scale <= 0.0)
    {
        return 1.0;
    }
    if (scale >= kLaplaceScales[kLaplaceChannels - 1])
    {
        return channel[kLaplaceChannels - 1];
    }
    if (scale <= kLaplaceScales[0])
    {
        // -ln T is ~linear in K for small K.
        const double depth = -std::log(std::max(channel[0], kFloor));
        return std::exp(-depth * scale / kLaplaceScales[0]);
    }
    int a = 0;
    while (a < kLaplaceChannels - 2 && scale > kLaplaceScales[a + 1])
    {
        ++a;
    }
    const double ya = -std::log(std::max(channel[a], kFloor));
    const double yb = -std::log(std::max(channel[a + 1], kFloor));
    const double u = (std::log(scale) - std::log(kLaplaceScales[a])) /
                     (std::log(kLaplaceScales[a + 1]) - std::log(kLaplaceScales[a]));
    if (ya < 1.0e-9 || yb < 1.0e-9)
    {
        return std::exp(-(ya + (yb - ya) * u));  // (near) empty footprint: linear is exact enough
    }
    return std::exp(-std::exp(std::log(ya) + (std::log(yb) - std::log(ya)) * u));
}

TransmittanceStats FromSamples(std::span<const double> unitDepths)
{
    TransmittanceStats out;
    if (unitDepths.empty())
    {
        return out;
    }
    out.channel.fill(0.0);
    for (const double s : unitDepths)
    {
        for (int j = 0; j < kLaplaceChannels; ++j)
        {
            out.channel[j] += std::exp(-kLaplaceScales[j] * s);
        }
        out.meanDepth += s;
    }
    const double inv = 1.0 / static_cast<double>(unitDepths.size());
    for (double& c : out.channel)
    {
        c *= inv;
    }
    out.meanDepth *= inv;
    return out;
}

TransmittanceStats Blend(std::span<const TransmittanceStats> parts, std::span<const double> weights)
{
    TransmittanceStats out;
    out.channel.fill(0.0);
    double total = 0.0;
    for (std::size_t i = 0; i < parts.size() && i < weights.size(); ++i)
    {
        for (int j = 0; j < kLaplaceChannels; ++j)
        {
            out.channel[j] += weights[i] * parts[i].channel[j];
        }
        out.meanDepth += weights[i] * parts[i].meanDepth;
        total += weights[i];
    }
    if (total <= 0.0)
    {
        return TransmittanceStats{};
    }
    for (double& c : out.channel)
    {
        c /= total;
    }
    out.meanDepth /= total;
    return out;
}

} // namespace orbit::optical_transport
