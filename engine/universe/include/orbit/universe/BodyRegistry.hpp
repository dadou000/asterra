#pragma once

#include <orbit/celestial_orbits/OrbitState.hpp>
#include <orbit/celestial_rotation/OrientationState.hpp>
#include <orbit/core/StrongId.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/math/RigidTransform.hpp>
#include <orbit/time/SimulationTime.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace orbit::universe
{
struct SystemIdTag;
struct BodyIdTag;

using SystemId = core::StrongId<SystemIdTag>;
using BodyId = core::StrongId<BodyIdTag>;

struct SphereShape
{
    f64 radiusMeters{1.0};
};

struct EllipsoidShape
{
    math::Double3 radiiMeters{
        1.0,
        1.0,
        1.0
    };
};

using BodyShape =
    std::variant<
        SphereShape,
        EllipsoidShape>;

[[nodiscard]] f64 ReferenceRadiusMeters(
    const BodyShape& shape) noexcept;

struct MassProperties
{
    f64 massKilograms{0.0};
};

struct FixedBodyTransform
{
    math::RigidTransformD parentFromBody{};
};

struct UniformRotationTransform
{
    math::Double3 centerInParentMeters{};
    math::Double3 axisInParent{
        0.0,
        0.0,
        1.0
    };
    f64 angularVelocityRadiansPerSecond{0.0};
    f64 phaseRadiansAtEpoch{0.0};
    time::SimulationTime epoch{};
};

// M05 compatibility bridge: orbital translation comes from the shared
// celestial-orbits state provider while orientation remains the existing
// uniform-spin model. M06 will formalize rotation as its own capability.
struct OrbitDrivenUniformRotationTransform
{
    std::shared_ptr<const celestial_orbits::OrbitStateProvider>
        orbitState;
    math::Double3 axisInParent{
        0.0,
        0.0,
        1.0
    };
    f64 angularVelocityRadiansPerSecond{0.0};
    f64 phaseRadiansAtEpoch{0.0};
    time::SimulationTime epoch{};
};

// M06 permanent composition path: orbital translation and body-fixed
// orientation are independent providers and are composed only when the
// FrameGraph asks for parentFromBody.
struct ProviderDrivenBodyTransform
{
    std::shared_ptr<const celestial_orbits::OrbitStateProvider>
        orbitState;
    std::shared_ptr<const celestial_rotation::OrientationProvider>
        orientation;
};

using BodyTransformModel =
    std::variant<
        FixedBodyTransform,
        UniformRotationTransform,
        OrbitDrivenUniformRotationTransform,
        ProviderDrivenBodyTransform>;

struct CelestialSystem
{
    SystemId id{};
    std::string name;
    frames::FrameId inertialFrame{};
};

struct CelestialBody
{
    BodyId id{};
    SystemId system{};
    std::string name;
    frames::FrameId frame{};
    frames::FrameId parentFrame{};
    BodyShape shape{};
    std::optional<MassProperties> mass;
    BodyTransformModel transformModel{};
};

struct BodyCreateDesc
{
    SystemId system{};
    std::string_view name;
    // Invalid means the system inertial frame.
    frames::FrameId parentFrame{};
    BodyShape shape{};
    std::optional<MassProperties> mass;
    BodyTransformModel transformModel{};
    // Invalid IDs request generated session identities. Persistent
    // composition layers provide stable IDs reconstructed from authority.
    BodyId id{};
    frames::FrameId frame{};
};

class BodyRegistry
{
public:
    explicit BodyRegistry(
        frames::FrameGraph& frameGraph);

    [[nodiscard]] SystemId CreateSystem(
        std::string_view name);

    [[nodiscard]] SystemId CreateSystem(
        std::string_view name,
        SystemId id,
        frames::FrameId inertialFrame);

    [[nodiscard]] BodyId CreateBody(
        const BodyCreateDesc& desc);

    [[nodiscard]] const CelestialSystem*
    FindSystem(SystemId id) const noexcept;

    [[nodiscard]] const CelestialBody*
    FindBody(BodyId id) const noexcept;

    [[nodiscard]] std::vector<SystemId>
    Systems() const;

    [[nodiscard]] std::vector<BodyId>
    Bodies(SystemId system) const;

private:
    frames::FrameGraph& frameGraph_;
    std::unordered_map<SystemId, CelestialSystem>
        systems_;
    std::unordered_map<BodyId, CelestialBody>
        bodies_;
};
} // namespace orbit::universe
