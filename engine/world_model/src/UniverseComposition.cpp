#include <orbit/world_model/UniverseComposition.hpp>

#include <orbit/celestial_orbits/OrbitState.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace orbit::world_model
{
namespace
{
template <typename TargetId>
[[nodiscard]] TargetId DerivedId(
    const scene::ObjectId source,
    const u64 highSalt,
    const u64 lowSalt) noexcept
{
    TargetId result{
        .high = source.high ^ highSalt,
        .low = source.low ^ lowSalt
    };

    if (!result)
    {
        result.low = 1;
    }

    return result;
}

template <typename Value>
[[nodiscard]] Value PropertyOr(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    Value fallback)
{
    const auto stored =
        objects.GetProperty(object, property);

    if (!stored.has_value())
    {
        return fallback;
    }

    const auto* value =
        std::get_if<Value>(&*stored);

    if (value == nullptr)
    {
        throw std::runtime_error(
            "World semantic property has an unexpected persisted type on object " +
            object.ToString() + ".");
    }

    return *value;
}

[[nodiscard]] universe::BodyShape BodyShapeFor(
    const scene::ObjectStore& objects,
    const scene::ObjectId object)
{
    const f64 equatorialRadius =
        PropertyOr<f64>(
            objects,
            object,
            kBodyRadius,
            6'000'000.0);

    const bool ellipsoid =
        PropertyOr<bool>(
            objects,
            object,
            kBodyEllipsoidEnabled,
            false);

    if (!ellipsoid)
    {
        return universe::SphereShape{
            .radiusMeters = equatorialRadius
        };
    }

    const f64 polarRadius =
        PropertyOr<f64>(
            objects,
            object,
            kBodyPolarRadius,
            equatorialRadius);

    return universe::EllipsoidShape{
        .radiiMeters = {
            equatorialRadius,
            equatorialRadius,
            polarRadius
        }
    };
}

[[nodiscard]] std::optional<scene::ObjectRecord>
FindOrbitCapability(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    std::optional<scene::ObjectRecord> found;

    for (const auto& child : objects.Children(body))
    {
        if (child.type != kOrbitCapabilityType)
        {
            continue;
        }

        const bool enabled =
            PropertyOr<bool>(
                objects,
                child.id,
                kCapabilityEnabled,
                true);

        if (!enabled)
        {
            continue;
        }

        if (found.has_value())
        {
            throw std::runtime_error(
                "Celestial body has multiple enabled orbit capabilities.");
        }

        found = child;
    }

    return found;
}

[[nodiscard]] std::shared_ptr<
    const celestial_orbits::OrbitStateProvider>
OrbitProviderFor(
    const scene::ObjectStore& objects,
    const scene::ObjectId body,
    const time::SimulationTime systemEpoch)
{
    const auto orbitCapability =
        FindOrbitCapability(objects, body);

    if (!orbitCapability.has_value())
    {
        return std::make_shared<
            celestial_orbits::FixedOrbitStateProvider>(
                celestial_orbits::FixedOrbitState{
                    .positionMeters =
                        PropertyOr<math::Double3>(
                            objects,
                            body,
                            kBodyParentPositionMeters,
                            {})
                });
    }

    const std::string model =
        PropertyOr<std::string>(
            objects,
            orbitCapability->id,
            kCapabilityModel,
            std::string{"Fixed"});

    if (model == "Fixed")
    {
        return std::make_shared<
            celestial_orbits::FixedOrbitStateProvider>(
                celestial_orbits::FixedOrbitState{
                    .positionMeters =
                        PropertyOr<math::Double3>(
                            objects,
                            body,
                            kBodyParentPositionMeters,
                            {})
                });
    }

    if (model != "Analytic Conic")
    {
        throw std::runtime_error(
            "Unsupported orbit capability model: " +
            model);
    }

    const f64 degreesToRadians =
        std::numbers::pi_v<f64> / 180.0;
    const i64 orbitEpochMicroseconds =
        PropertyOr<i64>(
            objects,
            orbitCapability->id,
            kOrbitEpochMicroseconds,
            systemEpoch.microsecondsFromEpoch);

    const f64 eccentricity =
        PropertyOr<f64>(
            objects,
            orbitCapability->id,
            kOrbitEccentricity,
            0.0);

    const f64 phase =
        std::abs(eccentricity - 1.0) <= 1.0e-10
            ? PropertyOr<f64>(
                objects,
                orbitCapability->id,
                kOrbitBarkerParameterEpoch,
                0.0)
            : PropertyOr<f64>(
                objects,
                orbitCapability->id,
                kOrbitMeanAnomalyEpochDegrees,
                0.0) *
              degreesToRadians;

    return std::make_shared<
        celestial_orbits::AnalyticConicOrbitStateProvider>(
            celestial_orbits::AnalyticConicElements{
                .semiMajorAxisMeters =
                    PropertyOr<f64>(
                        objects,
                        orbitCapability->id,
                        kOrbitSemiMajorAxisMeters,
                        1.0),
                .periapsisDistanceMeters =
                    PropertyOr<f64>(
                        objects,
                        orbitCapability->id,
                        kOrbitPeriapsisDistanceMeters,
                        1.0),
                .eccentricity = eccentricity,
                .inclinationRadians =
                    PropertyOr<f64>(
                        objects,
                        orbitCapability->id,
                        kOrbitInclinationDegrees,
                        0.0) *
                    degreesToRadians,
                .longitudeAscendingNodeRadians =
                    PropertyOr<f64>(
                        objects,
                        orbitCapability->id,
                        kOrbitAscendingNodeDegrees,
                        0.0) *
                    degreesToRadians,
                .argumentPeriapsisRadians =
                    PropertyOr<f64>(
                        objects,
                        orbitCapability->id,
                        kOrbitArgumentPeriapsisDegrees,
                        0.0) *
                    degreesToRadians,
                .phaseAtEpoch = phase,
                .gravitationalParameterM3PerS2 =
                    PropertyOr<f64>(
                        objects,
                        orbitCapability->id,
                        kOrbitGravitationalParameter,
                        1.0),
                .epoch = {
                    .microsecondsFromEpoch =
                        orbitEpochMicroseconds
                }
            });
}

[[nodiscard]] universe::BodyTransformModel
TransformFor(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const time::SimulationTime epoch)
{
    const f64 periodSeconds =
        PropertyOr<f64>(
            objects,
            object,
            kBodyRotationPeriodSeconds,
            86'400.0);

    const f64 tiltRadians =
        PropertyOr<f64>(
            objects,
            object,
            kBodyAxialTiltDegrees,
            0.0) *
        std::numbers::pi_v<f64> /
        180.0;

    const f64 phaseRadians =
        PropertyOr<f64>(
            objects,
            object,
            kBodyRotationPhaseDegrees,
            0.0) *
        std::numbers::pi_v<f64> /
        180.0;

    const f64 angularVelocity =
        periodSeconds > 0.0
            ? (2.0 * std::numbers::pi_v<f64>) /
                periodSeconds
            : 0.0;

    return universe::OrbitDrivenUniformRotationTransform{
        .orbitState =
            OrbitProviderFor(
                objects,
                object,
                epoch),
        .axisInParent = {
            0.0,
            std::sin(tiltRadians),
            std::cos(tiltRadians)
        },
        .angularVelocityRadiansPerSecond =
            angularVelocity,
        .phaseRadiansAtEpoch =
            phaseRadians,
        .epoch = epoch
    };
}
} // namespace

UniverseComposition::UniverseComposition()
    : frames_(
          std::make_unique<frames::FrameGraph>()),
      bodies_(
          std::make_unique<universe::BodyRegistry>(
              *frames_))
{
}

UniverseComposition::~UniverseComposition() = default;
UniverseComposition::UniverseComposition(
    UniverseComposition&&) noexcept = default;
UniverseComposition& UniverseComposition::operator=(
    UniverseComposition&&) noexcept = default;

UniverseCompositionStats UniverseComposition::Rebuild(
    const scene::ObjectStore& objects)
{
    auto candidateFrames =
        std::make_unique<frames::FrameGraph>();
    auto candidateBodies =
        std::make_unique<universe::BodyRegistry>(
            *candidateFrames);

    std::unordered_map<scene::ObjectId, universe::SystemId>
        candidateSystems;
    std::unordered_map<scene::ObjectId, universe::BodyId>
        candidateBodyIds;
    std::unordered_map<scene::ObjectId, frames::FrameId>
        candidateFrameIds;
    std::unordered_map<universe::BodyId, scene::ObjectId>
        candidateObjects;

    std::vector<scene::ObjectRecord> pending =
        objects.Roots();
    std::vector<scene::ObjectRecord> systemObjects;

    while (!pending.empty())
    {
        const auto object = pending.back();
        pending.pop_back();

        if (object.type == kCelestialSystemType)
        {
            systemObjects.push_back(object);
        }

        auto children =
            objects.Children(object.id);
        pending.insert(
            pending.end(),
            children.begin(),
            children.end());
    }

    u32 bodyCount = 0;
    u32 referenceNodeCount = 0;

    for (const auto& systemObject : systemObjects)
    {
        const universe::SystemId systemId =
            DerivedId<universe::SystemId>(
                systemObject.id,
                0x53595354454d4944ULL,
                0x4f52424954563033ULL);
        const frames::FrameId systemFrame =
            DerivedId<frames::FrameId>(
                systemObject.id,
                0x5359534652414d45ULL,
                0x4f52424954563033ULL);

        static_cast<void>(
            candidateBodies->CreateSystem(
                systemObject.name,
                systemId,
                systemFrame));

        candidateSystems.emplace(
            systemObject.id,
            systemId);
        candidateFrameIds.emplace(
            systemObject.id,
            systemFrame);

        const i64 epochMicroseconds =
            PropertyOr<i64>(
                objects,
                systemObject.id,
                kSystemEpochMicroseconds,
                i64{0});
        const time::SimulationTime epoch{
            .microsecondsFromEpoch =
                epochMicroseconds
        };

        std::function<void(
            const scene::ObjectRecord&,
            frames::FrameId)> composeNode;

        composeNode =
            [&](const scene::ObjectRecord& object,
                const frames::FrameId parentFrame)
            {
                frames::FrameId childParentFrame =
                    parentFrame;

                if (object.type ==
                    kCelestialReferenceNodeType)
                {
                    const frames::FrameId referenceFrame =
                        DerivedId<frames::FrameId>(
                            object.id,
                            0x5245464652414d45ULL,
                            0x4f52424954563036ULL);

                    const math::Double3 position =
                        PropertyOr<math::Double3>(
                            objects,
                            object.id,
                            kReferenceNodePositionMeters,
                            {});

                    static_cast<void>(
                        candidateFrames->CreateFrame(
                            referenceFrame,
                            parentFrame,
                            [position](
                                const time::SimulationTime)
                            {
                                return math::RigidTransformD{
                                    .translation = position
                                };
                            }));

                    candidateFrameIds.emplace(
                        object.id,
                        referenceFrame);
                    childParentFrame =
                        referenceFrame;
                    ++referenceNodeCount;
                }
                else if (object.type ==
                         kCelestialBodyType)
                {
                    const universe::BodyId bodyId =
                        DerivedId<universe::BodyId>(
                            object.id,
                            0x424f445949440003ULL,
                            0x4f52424954563033ULL);
                    const frames::FrameId bodyFrame =
                        DerivedId<frames::FrameId>(
                            object.id,
                            0x424f44594652414dULL,
                            0x4f52424954563033ULL);

                    const f64 massKilograms =
                        PropertyOr<f64>(
                            objects,
                            object.id,
                            kBodyMass,
                            5.0e24);

                    static_cast<void>(
                        candidateBodies->CreateBody({
                            .system = systemId,
                            .name = object.name,
                            .parentFrame = parentFrame,
                            .shape = BodyShapeFor(
                                objects,
                                object.id),
                            .mass =
                                universe::MassProperties{
                                    .massKilograms =
                                        massKilograms
                                },
                            .transformModel =
                                TransformFor(
                                    objects,
                                    object.id,
                                    epoch),
                            .id = bodyId,
                            .frame = bodyFrame
                        }));

                    candidateBodyIds.emplace(
                        object.id,
                        bodyId);
                    candidateFrameIds.emplace(
                        object.id,
                        bodyFrame);
                    candidateObjects.emplace(
                        bodyId,
                        object.id);
                    childParentFrame =
                        bodyFrame;
                    ++bodyCount;
                }
                else
                {
                    return;
                }

                for (const auto& child :
                     objects.Children(object.id))
                {
                    if (child.type ==
                            kCelestialBodyType ||
                        child.type ==
                            kCelestialReferenceNodeType)
                    {
                        composeNode(
                            child,
                            childParentFrame);
                    }
                }
            };

        for (const auto& child :
             objects.Children(systemObject.id))
        {
            if (child.type ==
                    kCelestialBodyType ||
                child.type ==
                    kCelestialReferenceNodeType)
            {
                composeNode(
                    child,
                    systemFrame);
            }
        }
    }

    frames_ = std::move(candidateFrames);
    bodies_ = std::move(candidateBodies);
    systemByObject_ =
        std::move(candidateSystems);
    bodyByObject_ =
        std::move(candidateBodyIds);
    frameByObject_ =
        std::move(candidateFrameIds);
    objectByBody_ =
        std::move(candidateObjects);
    sourceRevision_ = objects.Revision();

    return {
        .systems =
            static_cast<u32>(systemByObject_.size()),
        .referenceNodes = referenceNodeCount,
        .bodies = bodyCount,
        .sourceRevision = sourceRevision_
    };
}

bool UniverseComposition::RebuildIfChanged(
    const scene::ObjectStore& objects)
{
    if (sourceRevision_ == objects.Revision())
    {
        return false;
    }

    static_cast<void>(Rebuild(objects));
    return true;
}

frames::FrameGraph& UniverseComposition::Frames() noexcept
{
    return *frames_;
}

const frames::FrameGraph& UniverseComposition::Frames() const noexcept
{
    return *frames_;
}

universe::BodyRegistry& UniverseComposition::Bodies() noexcept
{
    return *bodies_;
}

const universe::BodyRegistry& UniverseComposition::Bodies() const noexcept
{
    return *bodies_;
}

std::optional<universe::SystemId>
UniverseComposition::SystemForObject(
    const scene::ObjectId object) const noexcept
{
    const auto found = systemByObject_.find(object);
    return found == systemByObject_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<universe::BodyId>
UniverseComposition::BodyForObject(
    const scene::ObjectId object) const noexcept
{
    const auto found = bodyByObject_.find(object);
    return found == bodyByObject_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<frames::FrameId>
UniverseComposition::FrameForObject(
    const scene::ObjectId object) const noexcept
{
    const auto found = frameByObject_.find(object);
    return found == frameByObject_.end()
        ? std::nullopt
        : std::optional(found->second);
}

std::optional<scene::ObjectId>
UniverseComposition::ObjectForBody(
    const universe::BodyId body) const noexcept
{
    const auto found = objectByBody_.find(body);
    return found == objectByBody_.end()
        ? std::nullopt
        : std::optional(found->second);
}

u64 UniverseComposition::SourceRevision() const noexcept
{
    return sourceRevision_;
}
} // namespace orbit::world_model
