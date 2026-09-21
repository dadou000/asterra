#pragma once

#include <orbit/celestial_orbits/OrbitState.hpp>
#include <orbit/core/StrongId.hpp>

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace orbit::celestial_orbits
{
struct NBodyMemberIdTag;
using NBodyMemberId = core::StrongId<NBodyMemberIdTag>;

struct NBodyMemberSeed
{
    NBodyMemberId id{};
    f64 massKilograms{0.0};
    std::shared_ptr<const OrbitStateProvider> sourceProvider;
};

struct NBodyIntegrationSettings
{
    // Fixed deterministic integration step. Queries may evaluate a final
    // fractional step after the last full step without mutating authority.
    f64 stepSeconds{60.0};
    f64 softeningMeters{0.0};
};

class NBodyDomain;

class NBodyOrbitStateProvider final : public OrbitStateProvider
{
public:
    NBodyOrbitStateProvider(
        std::shared_ptr<const NBodyDomain> domain,
        NBodyMemberId member);

    [[nodiscard]] OrbitState EvaluateState(
        time::SimulationTime atTime) const override;

    [[nodiscard]] std::string_view ModelName() const noexcept override;

private:
    std::shared_ptr<const NBodyDomain> domain_;
    NBodyMemberId member_{};
};

class NBodyDomain final :
    public std::enable_shared_from_this<NBodyDomain>
{
public:
    NBodyDomain(
        time::SimulationTime epoch,
        std::vector<NBodyMemberSeed> members,
        NBodyIntegrationSettings settings = {});

    [[nodiscard]] OrbitState EvaluateMember(
        NBodyMemberId member,
        time::SimulationTime atTime) const;

    [[nodiscard]] std::shared_ptr<const OrbitStateProvider>
    ProviderFor(NBodyMemberId member) const;

    [[nodiscard]] std::optional<OrbitState>
    DemotionState(
        NBodyMemberId member,
        time::SimulationTime atTime) const;

    [[nodiscard]] bool Contains(
        NBodyMemberId member) const noexcept;

    [[nodiscard]] time::SimulationTime Epoch() const noexcept;
    [[nodiscard]] const NBodyIntegrationSettings&
    Settings() const noexcept;

private:
    struct Member
    {
        NBodyMemberId id{};
        f64 massKilograms{0.0};
        OrbitState initialState{};
    };

    [[nodiscard]] std::vector<OrbitState>
    IntegrateTo(time::SimulationTime atTime) const;

    time::SimulationTime epoch_{};
    NBodyIntegrationSettings settings_{};
    std::vector<Member> members_;
    std::unordered_map<NBodyMemberId, std::size_t> indexById_;
};

inline constexpr f64 kGravitationalConstant =
    6.67430e-11;
} // namespace orbit::celestial_orbits
