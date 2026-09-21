#include <orbit/celestial_orbits/NBodyDomain.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::celestial_orbits
{
namespace
{
[[nodiscard]] bool Finite3(
    const math::Double3 value) noexcept
{
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z);
}

[[nodiscard]] std::vector<math::Double3>
Accelerations(
    const std::vector<OrbitState>& states,
    const std::vector<f64>& masses,
    const f64 softeningMeters)
{
    std::vector<math::Double3>
        acceleration(states.size());

    const f64 epsilon2 =
        softeningMeters * softeningMeters;

    for (std::size_t i = 0;
         i < states.size();
         ++i)
    {
        for (std::size_t j = i + 1;
             j < states.size();
             ++j)
        {
            const math::Double3 delta =
                states[j].positionMeters -
                states[i].positionMeters;

            const f64 distance2 =
                math::Dot(delta, delta) +
                epsilon2;

            if (distance2 <= 0.0)
            {
                throw std::runtime_error(
                    "N-body domain contains collocated singular members.");
            }

            const f64 inverseDistance =
                1.0 / std::sqrt(distance2);
            const f64 inverseDistance3 =
                inverseDistance *
                inverseDistance *
                inverseDistance;

            const math::Double3 directionScale =
                delta *
                (kGravitationalConstant *
                 inverseDistance3);

            acceleration[i] =
                acceleration[i] +
                directionScale * masses[j];

            acceleration[j] =
                acceleration[j] -
                directionScale * masses[i];
        }
    }

    return acceleration;
}

void VelocityVerletStep(
    std::vector<OrbitState>& states,
    const std::vector<f64>& masses,
    const f64 softeningMeters,
    const f64 dt)
{
    const auto a0 =
        Accelerations(
            states,
            masses,
            softeningMeters);

    for (std::size_t i = 0;
         i < states.size();
         ++i)
    {
        states[i].positionMeters =
            states[i].positionMeters +
            states[i].velocityMetersPerSecond * dt +
            a0[i] * (0.5 * dt * dt);
    }

    const auto a1 =
        Accelerations(
            states,
            masses,
            softeningMeters);

    for (std::size_t i = 0;
         i < states.size();
         ++i)
    {
        states[i].velocityMetersPerSecond =
            states[i].velocityMetersPerSecond +
            (a0[i] + a1[i]) *
                (0.5 * dt);

        states[i].quality =
            OrbitStateQuality::DynamicIntegrated;
    }
}
} // namespace

NBodyOrbitStateProvider::NBodyOrbitStateProvider(
    std::shared_ptr<const NBodyDomain> domain,
    const NBodyMemberId member)
    : domain_(std::move(domain)),
      member_(member)
{
    if (!domain_ ||
        !domain_->Contains(member_))
    {
        throw std::invalid_argument(
            "N-body provider requires a valid domain member.");
    }
}

OrbitState
NBodyOrbitStateProvider::EvaluateState(
    const time::SimulationTime atTime) const
{
    return domain_->
        EvaluateMember(
            member_,
            atTime);
}

std::string_view
NBodyOrbitStateProvider::ModelName() const noexcept
{
    return "Dynamic N-Body";
}

NBodyDomain::NBodyDomain(
    const time::SimulationTime epoch,
    std::vector<NBodyMemberSeed> members,
    const NBodyIntegrationSettings settings)
    : epoch_(epoch),
      settings_(settings)
{
    if (!std::isfinite(settings_.stepSeconds) ||
        settings_.stepSeconds <= 0.0 ||
        !std::isfinite(settings_.softeningMeters) ||
        settings_.softeningMeters < 0.0)
    {
        throw std::invalid_argument(
            "N-body integration settings are invalid.");
    }

    if (members.empty())
    {
        throw std::invalid_argument(
            "N-body domain requires at least one member.");
    }

    members_.reserve(members.size());
    indexById_.reserve(members.size());

    for (const auto& seed : members)
    {
        if (!seed.id ||
            !seed.sourceProvider ||
            !std::isfinite(seed.massKilograms) ||
            seed.massKilograms < 0.0)
        {
            throw std::invalid_argument(
                "N-body member seed is invalid.");
        }

        if (indexById_.contains(seed.id))
        {
            throw std::invalid_argument(
                "N-body member IDs must be unique.");
        }

        const OrbitState state =
            seed.sourceProvider->
                EvaluateState(epoch_);

        if (!Finite3(state.positionMeters) ||
            !Finite3(state.velocityMetersPerSecond))
        {
            throw std::invalid_argument(
                "N-body promotion state is non-finite.");
        }

        indexById_.emplace(
            seed.id,
            members_.size());

        members_.push_back({
            .id = seed.id,
            .massKilograms =
                seed.massKilograms,
            .initialState = state
        });
    }
}

std::vector<OrbitState>
NBodyDomain::IntegrateTo(
    const time::SimulationTime atTime) const
{
    std::vector<OrbitState> states;
    states.reserve(members_.size());

    std::vector<f64> masses;
    masses.reserve(members_.size());

    for (const auto& member : members_)
    {
        states.push_back(
            member.initialState);
        masses.push_back(
            member.massKilograms);
    }

    const f64 totalSeconds =
        static_cast<f64>(
            atTime.microsecondsFromEpoch -
            epoch_.microsecondsFromEpoch) /
        1'000'000.0;

    if (totalSeconds == 0.0)
    {
        for (auto& state : states)
        {
            state.quality =
                OrbitStateQuality::DynamicIntegrated;
        }
        return states;
    }

    const f64 direction =
        totalSeconds > 0.0
            ? 1.0
            : -1.0;

    const f64 fullStep =
        settings_.stepSeconds *
        direction;

    const u64 fullSteps =
        static_cast<u64>(
            std::floor(
                std::abs(totalSeconds) /
                settings_.stepSeconds));

    for (u64 step = 0;
         step < fullSteps;
         ++step)
    {
        VelocityVerletStep(
            states,
            masses,
            settings_.softeningMeters,
            fullStep);
    }

    const f64 consumed =
        static_cast<f64>(fullSteps) *
        settings_.stepSeconds *
        direction;

    const f64 remainder =
        totalSeconds - consumed;

    if (remainder != 0.0)
    {
        VelocityVerletStep(
            states,
            masses,
            settings_.softeningMeters,
            remainder);
    }

    return states;
}

OrbitState NBodyDomain::EvaluateMember(
    const NBodyMemberId member,
    const time::SimulationTime atTime) const
{
    const auto found =
        indexById_.find(member);

    if (found == indexById_.end())
    {
        throw std::out_of_range(
            "N-body member is not part of this domain.");
    }

    return IntegrateTo(atTime)[
        found->second];
}

std::shared_ptr<const OrbitStateProvider>
NBodyDomain::ProviderFor(
    const NBodyMemberId member) const
{
    if (!Contains(member))
    {
        throw std::out_of_range(
            "Cannot create provider for unknown N-body member.");
    }

    return std::make_shared<
        NBodyOrbitStateProvider>(
            shared_from_this(),
            member);
}

std::optional<OrbitState>
NBodyDomain::DemotionState(
    const NBodyMemberId member,
    const time::SimulationTime atTime) const
{
    if (!Contains(member))
    {
        return std::nullopt;
    }

    return EvaluateMember(
        member,
        atTime);
}

bool NBodyDomain::Contains(
    const NBodyMemberId member) const noexcept
{
    return indexById_.contains(member);
}

time::SimulationTime
NBodyDomain::Epoch() const noexcept
{
    return epoch_;
}

const NBodyIntegrationSettings&
NBodyDomain::Settings() const noexcept
{
    return settings_;
}
} // namespace orbit::celestial_orbits
