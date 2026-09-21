#include <orbit/celestial_orbits/ImportedEphemeris.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace orbit::celestial_orbits
{
namespace
{
[[nodiscard]] f64 SecondsBetween(
    const time::SimulationTime a,
    const time::SimulationTime b) noexcept
{
    return static_cast<f64>(
        b.microsecondsFromEpoch -
        a.microsecondsFromEpoch) /
        1'000'000.0;
}

void ValidateSample(
    const EphemerisSample& sample)
{
    const auto finite3 =
        [](const math::Double3 value)
        {
            return std::isfinite(value.x) &&
                   std::isfinite(value.y) &&
                   std::isfinite(value.z);
        };

    if (!finite3(sample.positionMeters) ||
        !finite3(sample.velocityMetersPerSecond))
    {
        throw std::invalid_argument(
            "Imported ephemeris sample contains non-finite state.");
    }
}

[[nodiscard]] math::Double3 HermitePosition(
    const EphemerisSample& a,
    const EphemerisSample& b,
    const f64 u,
    const f64 dtSeconds) noexcept
{
    const f64 u2 = u * u;
    const f64 u3 = u2 * u;

    const f64 h00 =
        2.0 * u3 - 3.0 * u2 + 1.0;
    const f64 h10 =
        u3 - 2.0 * u2 + u;
    const f64 h01 =
        -2.0 * u3 + 3.0 * u2;
    const f64 h11 =
        u3 - u2;

    return
        a.positionMeters * h00 +
        a.velocityMetersPerSecond *
            (h10 * dtSeconds) +
        b.positionMeters * h01 +
        b.velocityMetersPerSecond *
            (h11 * dtSeconds);
}

[[nodiscard]] math::Double3 HermiteVelocity(
    const EphemerisSample& a,
    const EphemerisSample& b,
    const f64 u,
    const f64 dtSeconds) noexcept
{
    const f64 u2 = u * u;

    const f64 dh00 =
        6.0 * u2 - 6.0 * u;
    const f64 dh10 =
        3.0 * u2 - 4.0 * u + 1.0;
    const f64 dh01 =
        -6.0 * u2 + 6.0 * u;
    const f64 dh11 =
        3.0 * u2 - 2.0 * u;

    return
        a.positionMeters *
            (dh00 / dtSeconds) +
        a.velocityMetersPerSecond * dh10 +
        b.positionMeters *
            (dh01 / dtSeconds) +
        b.velocityMetersPerSecond * dh11;
}
} // namespace

ImportedEphemerisProvider::ImportedEphemerisProvider(
    std::vector<EphemerisSample> samples,
    std::string sourceName)
    : samples_(std::move(samples)),
      sourceName_(std::move(sourceName))
{
    if (samples_.empty())
    {
        throw std::invalid_argument(
            "Imported ephemeris requires at least one sample.");
    }

    std::sort(
        samples_.begin(),
        samples_.end(),
        [](const EphemerisSample& lhs,
           const EphemerisSample& rhs)
        {
            return lhs.time.microsecondsFromEpoch <
                rhs.time.microsecondsFromEpoch;
        });

    for (std::size_t index = 0;
         index < samples_.size();
         ++index)
    {
        ValidateSample(samples_[index]);

        if (index > 0 &&
            samples_[index - 1].
                time.microsecondsFromEpoch ==
            samples_[index].
                time.microsecondsFromEpoch)
        {
            throw std::invalid_argument(
                "Imported ephemeris contains duplicate timestamps.");
        }
    }

    range_ = {
        .first = samples_.front().time,
        .last = samples_.back().time
    };
}

OrbitState ImportedEphemerisProvider::EvaluateState(
    const time::SimulationTime atTime) const
{
    if (!ContainsTime(atTime))
    {
        throw std::out_of_range(
            "Imported ephemeris query " +
            std::to_string(atTime.microsecondsFromEpoch) +
            " us is outside valid range [" +
            std::to_string(range_.first.microsecondsFromEpoch) +
            ", " +
            std::to_string(range_.last.microsecondsFromEpoch) +
            "] us for source '" +
            sourceName_ + "'.");
    }

    const auto lower =
        std::lower_bound(
            samples_.begin(),
            samples_.end(),
            atTime.microsecondsFromEpoch,
            [](const EphemerisSample& sample,
               const i64 query)
            {
                return sample.time.
                    microsecondsFromEpoch < query;
            });

    if (lower != samples_.end() &&
        lower->time.microsecondsFromEpoch ==
            atTime.microsecondsFromEpoch)
    {
        return {
            .positionMeters = lower->positionMeters,
            .velocityMetersPerSecond =
                lower->velocityMetersPerSecond,
            .quality =
                OrbitStateQuality::SampledExact
        };
    }

    if (samples_.size() == 1)
    {
        return {
            .positionMeters =
                samples_.front().positionMeters,
            .velocityMetersPerSecond =
                samples_.front().
                    velocityMetersPerSecond,
            .quality =
                OrbitStateQuality::SampledExact
        };
    }

    const auto right = lower;
    const auto left = std::prev(lower);

    const f64 dtSeconds =
        SecondsBetween(
            left->time,
            right->time);
    const f64 querySeconds =
        SecondsBetween(
            left->time,
            atTime);
    const f64 u =
        querySeconds / dtSeconds;

    return {
        .positionMeters =
            HermitePosition(
                *left,
                *right,
                u,
                dtSeconds),
        .velocityMetersPerSecond =
            HermiteVelocity(
                *left,
                *right,
                u,
                dtSeconds),
        .quality =
            OrbitStateQuality::SampledInterpolated
    };
}

std::string_view
ImportedEphemerisProvider::ModelName() const noexcept
{
    return "Imported Ephemeris";
}

const EphemerisRange&
ImportedEphemerisProvider::Range() const noexcept
{
    return range_;
}

bool ImportedEphemerisProvider::ContainsTime(
    const time::SimulationTime atTime) const noexcept
{
    return atTime.microsecondsFromEpoch >=
               range_.first.microsecondsFromEpoch &&
           atTime.microsecondsFromEpoch <=
               range_.last.microsecondsFromEpoch;
}

std::string_view
ImportedEphemerisProvider::SourceName() const noexcept
{
    return sourceName_;
}
} // namespace orbit::celestial_orbits
