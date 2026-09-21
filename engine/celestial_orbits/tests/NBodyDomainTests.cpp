#include <orbit/celestial_orbits/NBodyDomain.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

namespace
{
bool Near(
    double a,
    double b,
    double rel)
{
    const double scale =
        std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <=
        rel * scale;
}
} // namespace

int main()
{
    using namespace orbit::celestial_orbits;

    constexpr double sunMass = 1.98847e30;
    constexpr double earthMass = 5.9722e24;
    constexpr double radius = 149597870700.0;

    const double orbitalVelocity =
        std::sqrt(
            kGravitationalConstant *
            sunMass / radius);

    const auto fixedSun =
        std::make_shared<FixedOrbitStateProvider>(
            FixedOrbitState{
                .positionMeters = {0.0, 0.0, 0.0}
            });

    class MovingSeed final : public OrbitStateProvider
    {
    public:
        OrbitState EvaluateState(
            orbit::time::SimulationTime) const override
        {
            return {
                .positionMeters =
                    {radius, 0.0, 0.0},
                .velocityMetersPerSecond =
                    {0.0, orbitalVelocity, 0.0},
                .quality =
                    OrbitStateQuality::Fixed
            };
        }

        std::string_view ModelName() const noexcept override
        {
            return "Seed";
        }
    };

    const auto movingEarth =
        std::make_shared<MovingSeed>();

    const NBodyMemberId sunId{
        .high = 1,
        .low = 1
    };
    const NBodyMemberId earthId{
        .high = 2,
        .low = 2
    };

    auto domain =
        std::make_shared<NBodyDomain>(
            orbit::time::SimulationTime{},
            std::vector<NBodyMemberSeed>{
                {
                    .id = sunId,
                    .massKilograms = sunMass,
                    .sourceProvider = fixedSun
                },
                {
                    .id = earthId,
                    .massKilograms = earthMass,
                    .sourceProvider = movingEarth
                }
            },
            NBodyIntegrationSettings{
                .stepSeconds = 3600.0,
                .softeningMeters = 0.0
            });

    const auto earthProvider =
        domain->ProviderFor(earthId);

    const auto initial =
        earthProvider->EvaluateState(
            orbit::time::SimulationTime{});

    if (initial.quality !=
            OrbitStateQuality::DynamicIntegrated ||
        initial.positionMeters.x != radius ||
        initial.velocityMetersPerSecond.y !=
            orbitalVelocity)
    {
        return 1;
    }

    const double period =
        2.0 * std::numbers::pi_v<double> *
        std::sqrt(
            radius * radius * radius /
            (kGravitationalConstant *
             (sunMass + earthMass)));

    const auto quarter =
        earthProvider->EvaluateState(
            orbit::time::SimulationTime{
                .microsecondsFromEpoch =
                    static_cast<orbit::i64>(
                        period * 0.25 *
                        1'000'000.0)
            });

    if (!Near(
            std::sqrt(
                math::Dot(
                    quarter.positionMeters,
                    quarter.positionMeters)),
            radius,
            5.0e-3))
    {
        return 2;
    }

    const auto demoted =
        domain->DemotionState(
            earthId,
            orbit::time::SimulationTime{
                .microsecondsFromEpoch =
                    86'400'000'000LL
            });

    if (!demoted.has_value() ||
        demoted->quality !=
            OrbitStateQuality::DynamicIntegrated)
    {
        return 3;
    }

    const auto reverse =
        earthProvider->EvaluateState(
            orbit::time::SimulationTime{
                .microsecondsFromEpoch =
                    -86'400'000'000LL
            });

    if (!std::isfinite(reverse.positionMeters.x) ||
        !std::isfinite(reverse.velocityMetersPerSecond.y))
    {
        return 4;
    }

    return 0;
}
