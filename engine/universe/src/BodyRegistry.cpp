#include <orbit/universe/BodyRegistry.hpp>

#include <orbit/math/Vector.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace orbit::universe
{
namespace
{
[[nodiscard]] math::Double3 RotateAroundAxis(
    const math::Double3& value,
    const math::Double3& axis,
    const f64 radians) noexcept
{
    const f64 c = std::cos(radians);
    const f64 s = std::sin(radians);

    return value * c +
        math::Cross(axis, value) * s +
        axis *
            (math::Dot(axis, value) *
             (1.0 - c));
}

[[nodiscard]] math::RigidTransformD EvaluateCenterTransform(
    const BodyTransformModel& model,
    const time::SimulationTime atTime)
{
    return std::visit(
        [atTime](const auto& value)
            -> math::RigidTransformD
        {
            using Model =
                std::decay_t<decltype(value)>;

            math::Double3 translation{};

            if constexpr (
                std::is_same_v<
                    Model,
                    FixedBodyTransform>)
            {
                translation =
                    value.parentFromBody.translation;
            }
            else if constexpr (
                std::is_same_v<
                    Model,
                    UniformRotationTransform>)
            {
                translation =
                    value.centerInParentMeters;
            }
            else if constexpr (
                std::is_same_v<
                    Model,
                    OrbitDrivenUniformRotationTransform>)
            {
                if (!value.orbitState)
                {
                    throw std::runtime_error(
                        "Orbit-driven body transform has no orbit state provider.");
                }

                translation =
                    value.orbitState->
                        EvaluateState(atTime).
                        positionMeters;
            }
            else
            {
                if (!value.orbitState)
                {
                    throw std::runtime_error(
                        "Provider-driven body transform has no orbit state provider.");
                }

                translation =
                    value.orbitState->
                        EvaluateState(atTime).
                        positionMeters;
            }

            return {
                .translation = translation
            };
        },
        model);
}

[[nodiscard]] math::RigidTransformD EvaluateBodyOrientation(
    const BodyTransformModel& model,
    const time::SimulationTime atTime)
{
    return std::visit(
        [atTime](const auto& value)
            -> math::RigidTransformD
        {
            using Model =
                std::decay_t<decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Model,
                    FixedBodyTransform>)
            {
                return {
                    .rotation =
                        value.parentFromBody.rotation
                };
            }
            else if constexpr (
                std::is_same_v<
                    Model,
                    ProviderDrivenBodyTransform>)
            {
                if (!value.orientation)
                {
                    throw std::runtime_error(
                        "Provider-driven body transform has no orientation provider.");
                }

                return {
                    .rotation =
                        value.orientation->
                            EvaluateOrientation(atTime).
                            parentFromBodyRotation
                };
            }
            else
            {
                const f64 elapsedSeconds =
                    static_cast<f64>(
                        (atTime - value.epoch).
                            count()) /
                    1'000'000.0;

                const f64 angle =
                    value.phaseRadiansAtEpoch +
                    value.angularVelocityRadiansPerSecond *
                        elapsedSeconds;

                const math::Double3 axis =
                    math::Normalize(
                        value.axisInParent);

                return {
                    .rotation = {
                        .xAxis =
                            RotateAroundAxis(
                                {1.0, 0.0, 0.0},
                                axis,
                                angle),
                        .yAxis =
                            RotateAroundAxis(
                                {0.0, 1.0, 0.0},
                                axis,
                                angle),
                        .zAxis =
                            RotateAroundAxis(
                                {0.0, 0.0, 1.0},
                                axis,
                                angle)
                    }
                };
            }
        },
        model);
}

void ValidateShape(
    const BodyShape& shape)
{
    std::visit(
        [](const auto& value)
        {
            using Shape =
                std::decay_t<
                    decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    SphereShape>)
            {
                if (!std::isfinite(
                        value.radiusMeters) ||
                    value.radiusMeters <= 0.0)
                {
                    throw std::invalid_argument(
                        "Sphere radius must be finite and positive.");
                }
            }
            else
            {
                const auto validRadius =
                    [](const f64 radius)
                    {
                        return
                            std::isfinite(radius) &&
                            radius > 0.0;
                    };

                if (!validRadius(
                        value.radiiMeters.x) ||
                    !validRadius(
                        value.radiiMeters.y) ||
                    !validRadius(
                        value.radiiMeters.z))
                {
                    throw std::invalid_argument(
                        "Ellipsoid radii must be finite and positive.");
                }
            }
        },
        shape);
}

void ValidateTransform(
    const BodyTransformModel& model)
{
    std::visit(
        [](const auto& value)
        {
            using Model =
                std::decay_t<decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Model,
                    ProviderDrivenBodyTransform>)
            {
                if (!value.orbitState ||
                    !value.orientation)
                {
                    throw std::invalid_argument(
                        "Provider-driven transform requires orbit and orientation providers.");
                }
            }
            else if constexpr (
                std::is_same_v<Model, UniformRotationTransform> ||
                std::is_same_v<Model, OrbitDrivenUniformRotationTransform>)
            {
                if (math::Length(
                        value.axisInParent) <= 0.0)
                {
                    throw std::invalid_argument(
                        "Uniform rotation axis must be non-zero.");
                }

                if (!std::isfinite(
                        value.angularVelocityRadiansPerSecond) ||
                    !std::isfinite(
                        value.phaseRadiansAtEpoch))
                {
                    throw std::invalid_argument(
                        "Uniform rotation parameters must be finite.");
                }

                if constexpr (
                    std::is_same_v<
                        Model,
                        OrbitDrivenUniformRotationTransform>)
                {
                    if (!value.orbitState)
                    {
                        throw std::invalid_argument(
                            "Orbit-driven transform requires an orbit state provider.");
                    }
                }
            }
        },
        model);
}
} // namespace

f64 ReferenceRadiusMeters(
    const BodyShape& shape) noexcept
{
    return std::visit(
        [](const auto& value) -> f64
        {
            using Shape =
                std::decay_t<
                    decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    SphereShape>)
            {
                return value.radiusMeters;
            }
            else
            {
                return std::max({
                    value.radiiMeters.x,
                    value.radiiMeters.y,
                    value.radiiMeters.z
                });
            }
        },
        shape);
}

BodyRegistry::BodyRegistry(
    frames::FrameGraph& frameGraph)
    : frameGraph_(frameGraph)
{
}

SystemId BodyRegistry::CreateSystem(
    const std::string_view name)
{
    SystemId id = SystemId::Random();

    while (systems_.contains(id))
    {
        id = SystemId::Random();
    }

    frames::FrameId inertialFrame =
        frames::FrameId::Random();

    while (frameGraph_.Contains(inertialFrame))
    {
        inertialFrame =
            frames::FrameId::Random();
    }

    return CreateSystem(
        name,
        id,
        inertialFrame);
}

SystemId BodyRegistry::CreateSystem(
    const std::string_view name,
    const SystemId id,
    const frames::FrameId inertialFrame)
{
    if (name.empty())
    {
        throw std::invalid_argument(
            "Celestial system name must not be empty.");
    }

    if (!id)
    {
        throw std::invalid_argument(
            "Celestial system ID must be valid.");
    }

    if (!inertialFrame)
    {
        throw std::invalid_argument(
            "Celestial system inertial frame ID must be valid.");
    }

    if (systems_.contains(id))
    {
        throw std::invalid_argument(
            "Celestial system ID already exists.");
    }

    if (frameGraph_.Contains(inertialFrame))
    {
        throw std::invalid_argument(
            "Celestial system inertial frame already exists.");
    }

    static_cast<void>(
        frameGraph_.CreateRoot(
            inertialFrame));

    CelestialSystem system{
        .id = id,
        .name = std::string(name),
        .inertialFrame =
            inertialFrame
    };

    systems_.emplace(
        id,
        std::move(system));

    return id;
}

BodyId BodyRegistry::CreateBody(
    const BodyCreateDesc& desc)
{
    const auto systemFound =
        systems_.find(desc.system);

    if (systemFound == systems_.end())
    {
        throw std::invalid_argument(
            "Body references an unknown celestial system.");
    }

    if (desc.name.empty())
    {
        throw std::invalid_argument(
            "Celestial body name must not be empty.");
    }

    ValidateShape(desc.shape);
    ValidateTransform(desc.transformModel);

    if (desc.mass.has_value() &&
        (!std::isfinite(
             desc.mass->massKilograms) ||
         desc.mass->massKilograms < 0.0))
    {
        throw std::invalid_argument(
            "Body mass must be finite and non-negative.");
    }

    const frames::FrameId parentFrame =
        desc.parentFrame
            ? desc.parentFrame
            : systemFound->
                second.inertialFrame;

    if (!frameGraph_.Contains(parentFrame))
    {
        throw std::invalid_argument(
            "Body parent frame does not exist.");
    }

    BodyId id =
        desc.id;

    if (!id)
    {
        id = BodyId::Random();

        while (bodies_.contains(id))
        {
            id = BodyId::Random();
        }
    }
    else if (bodies_.contains(id))
    {
        throw std::invalid_argument(
            "Celestial body ID already exists.");
    }

    frames::FrameId centerFrame =
        desc.centerFrame;

    if (!centerFrame)
    {
        centerFrame =
            frames::FrameId::Random();

        while (frameGraph_.Contains(centerFrame))
        {
            centerFrame =
                frames::FrameId::Random();
        }
    }
    else if (frameGraph_.Contains(centerFrame))
    {
        throw std::invalid_argument(
            "Celestial body center frame already exists.");
    }

    frames::FrameId bodyFrame =
        desc.frame;

    if (!bodyFrame)
    {
        bodyFrame =
            frames::FrameId::Random();

        while (frameGraph_.Contains(bodyFrame) ||
               bodyFrame == centerFrame)
        {
            bodyFrame =
                frames::FrameId::Random();
        }
    }
    else if (frameGraph_.Contains(bodyFrame) ||
             bodyFrame == centerFrame)
    {
        throw std::invalid_argument(
            "Celestial body frame already exists.");
    }

    const BodyTransformModel model =
        desc.transformModel;

    static_cast<void>(
        frameGraph_.CreateFrame(
            centerFrame,
            parentFrame,
            [model](
                const time::SimulationTime atTime)
            {
                return EvaluateCenterTransform(
                    model,
                    atTime);
            }));

    static_cast<void>(
        frameGraph_.CreateFrame(
            bodyFrame,
            centerFrame,
            [model](
                const time::SimulationTime atTime)
            {
                return EvaluateBodyOrientation(
                    model,
                    atTime);
            }));

    CelestialBody body{
        .id = id,
        .system = desc.system,
        .name = std::string(desc.name),
        .frame = bodyFrame,
        .centerFrame = centerFrame,
        .parentFrame = parentFrame,
        .shape = desc.shape,
        .mass = desc.mass,
        .transformModel =
            desc.transformModel
    };

    bodies_.emplace(
        id,
        std::move(body));

    return id;
}

const CelestialSystem*
BodyRegistry::FindSystem(
    const SystemId id) const noexcept
{
    const auto found = systems_.find(id);

    return found == systems_.end()
        ? nullptr
        : &found->second;
}

const CelestialBody*
BodyRegistry::FindBody(
    const BodyId id) const noexcept
{
    const auto found = bodies_.find(id);

    return found == bodies_.end()
        ? nullptr
        : &found->second;
}

std::vector<SystemId>
BodyRegistry::Systems() const
{
    std::vector<SystemId> result;
    result.reserve(systems_.size());

    for (const auto& [id, system] :
         systems_)
    {
        static_cast<void>(system);
        result.push_back(id);
    }

    return result;
}

std::vector<BodyId>
BodyRegistry::Bodies(
    const SystemId system) const
{
    std::vector<BodyId> result;

    for (const auto& [id, body] :
         bodies_)
    {
        if (body.system == system)
        {
            result.push_back(id);
        }
    }

    return result;
}
} // namespace orbit::universe
