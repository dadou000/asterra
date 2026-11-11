#include <orbit/storm_electrification/StormLightningSource.hpp>

#include <algorithm>
#include <utility>

namespace orbit::storm_electrification
{
StormLightningSource::StormLightningSource(
    StormFields fields, const std::uint32_t nx, const std::uint32_t ny, const std::uint32_t nz, const double spacingMeters,
    StormLightningParams params)
    : fields_(std::move(fields)), grid_(nx, ny, nz, spacingMeters), params_(std::move(params))
{
}

void RebaseFlashTimes(FlashResult& flash, const double stretch)
{
    const double start = flash.startTime;
    for (std::size_t i = 0; i < flash.graph.Segments().size(); ++i)
    {
        optical_transport::Segment& segment = flash.graph.At(static_cast<std::int32_t>(i));
        segment.formedAt = (segment.formedAt - start) * stretch;
        for (optical_transport::Pulse& pulse : segment.pulses)
        {
            pulse.startTime = (pulse.startTime - start) * stretch;
            pulse.duration *= stretch;
            pulse.radiantEnergy *= stretch;   // the same power for longer
        }
    }
    flash.endTime = (flash.endTime - start) * stretch;
    flash.startTime = 0.0;
}

double StormLightningSource::MaxBreakdownRatio() const
{
    return storm_electrification::MaxBreakdownRatio(grid_, params_.leader);
}

std::vector<FlashResult> StormLightningSource::Advance(const double wallSeconds, const std::uint32_t maxSteps)
{
    std::vector<FlashResult> out;
    carry_ += std::max(wallSeconds, 0.0) * params_.timeScale;
    std::uint32_t taken = 0;
    while (carry_ >= params_.stepSeconds)
    {
        if (maxSteps > 0U && taken == maxSteps)
        {
            carry_ = std::min(carry_, params_.stepSeconds * 0.5);   // drop the backlog
            break;
        }
        ++taken;
        carry_ -= params_.stepSeconds;
        StepElectrification(grid_, fields_, params_.electrification, params_.stepSeconds);
        simulated_ += params_.stepSeconds;
        grid_.SolvePotential();
        for (std::uint32_t guard = 0; guard < params_.maxFlashesPerStep && MaxBreakdownRatio() >= 1.0; ++guard)
        {
            FlashResult flash = TryFlash(grid_, params_.leader, simulated_, flashes_);
            if (!flash.occurred)
            {
                break;
            }
            ++flashes_;
            out.push_back(std::move(flash));
        }
    }
    return out;
}
} // namespace orbit::storm_electrification
