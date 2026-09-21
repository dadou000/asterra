#include <orbit/world_model/UniverseComposition.hpp>

#include <orbit/celestial_orbits/ImportedEphemeris.hpp>
#include <orbit/celestial_orbits/NBodyDomain.hpp>
#include <orbit/celestial_gravity/GravityService.hpp>
#include <orbit/celestial_orbits/OrbitState.hpp>
#include <orbit/celestial_rotation/OrientationState.hpp>

#include <orbit/world_model/CelestialCompactObjectBinding.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
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
    // Compact-object bodies do not require or imply a solid surface.
    // BodyShape remains the registry's conservative spatial/reference
    // envelope, so use the optical shadow scale instead of inventing a
    // physical body radius.
    if (const auto compact =
            ResolveCompactObject(
                objects,
                object);
        compact.has_value())
    {
        const auto presentation =
            celestial_compact_objects::
                BuildCompactObjectPresentation(
                    compact->parameters);

        return universe::SphereShape{
            .radiusMeters =
                std::max(
                    presentation.
                        shadowRadiusMeters,
                    1.0)
        };
    }

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

    if (model == "Imported Ephemeris")
    {
        const auto sourceReference =
            PropertyOr<schema::ObjectReferenceValue>(
                objects,
                orbitCapability->id,
                kCapabilitySourceObject,
                schema::ObjectReferenceValue{});

        const scene::ObjectId sourceObject{
            .high = sourceReference.high,
            .low = sourceReference.low
        };

        if (!sourceObject)
        {
            throw std::runtime_error(
                "Imported Ephemeris orbit requires a Source Object.");
        }

        const auto sourceRecord =
            objects.Find(sourceObject);

        if (!sourceRecord.has_value() ||
            sourceRecord->type != kEphemerisAssetType)
        {
            throw std::runtime_error(
                "Imported Ephemeris Source Object must reference an Ephemeris Asset.");
        }

        std::vector<
            celestial_orbits::EphemerisSample> samples;

        for (const auto& sample :
             objects.Children(sourceObject))
        {
            if (sample.type != kEphemerisSampleType)
            {
                continue;
            }

            samples.push_back({
                .time = {
                    .microsecondsFromEpoch =
                        PropertyOr<i64>(
                            objects,
                            sample.id,
                            kEphemerisSampleTimeMicroseconds,
                            i64{0})
                },
                .positionMeters =
                    PropertyOr<math::Double3>(
                        objects,
                        sample.id,
                        kEphemerisSamplePositionMeters,
                        {}),
                .velocityMetersPerSecond =
                    PropertyOr<math::Double3>(
                        objects,
                        sample.id,
                        kEphemerisSampleVelocityMetersPerSecond,
                        {})
            });
        }

        const std::string sourceLabel =
            PropertyOr<std::string>(
                objects,
                sourceObject,
                kEphemerisSourceLabel,
                sourceRecord->name);

        return std::make_shared<
            celestial_orbits::ImportedEphemerisProvider>(
                std::move(samples),
                sourceLabel);
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

struct DynamicPromotionSettings
{
    f64 stepSeconds{60.0};
    f64 softeningMeters{0.0};
};

[[nodiscard]] std::optional<DynamicPromotionSettings>
DynamicPromotionSettingsFor(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    const auto orbitCapability =
        FindOrbitCapability(objects, body);

    if (!orbitCapability.has_value())
    {
        return std::nullopt;
    }

    if (!PropertyOr<bool>(
            objects,
            orbitCapability->id,
            kOrbitDynamicPromotionEnabled,
            false))
    {
        return std::nullopt;
    }

    return DynamicPromotionSettings{
        .stepSeconds =
            PropertyOr<f64>(
                objects,
                orbitCapability->id,
                kOrbitDynamicStepSeconds,
                60.0),
        .softeningMeters =
            PropertyOr<f64>(
                objects,
                orbitCapability->id,
                kOrbitDynamicSofteningMeters,
                0.0)
    };
}

[[nodiscard]] celestial_orbits::NBodyMemberId
NBodyMemberForObject(
    const scene::ObjectId object) noexcept
{
    celestial_orbits::NBodyMemberId result{
        .high =
            object.high ^
            0x4e424f44594d454dULL,
        .low =
            object.low ^
            0x4f52424954563036ULL
    };

    if (!result)
    {
        result.low = 1;
    }

    return result;
}

struct GravityConfiguration
{
    f64 gravitationalParameterM3PerS2{0.0};
    f64 softeningMeters{0.0};
};

[[nodiscard]] std::optional<GravityConfiguration>
GravityConfigurationFor(
    const scene::ObjectStore& objects,
    const scene::ObjectId body,
    const f64 bodyMassKilograms)
{
    std::optional<scene::ObjectRecord> capability;

    for (const auto& child : objects.Children(body))
    {
        if (child.type != kGravityCapabilityType)
        {
            continue;
        }

        if (!PropertyOr<bool>(
                objects,
                child.id,
                kCapabilityEnabled,
                true))
        {
            continue;
        }

        if (capability.has_value())
        {
            throw std::runtime_error(
                "Celestial body has multiple enabled gravity capabilities.");
        }

        capability = child;
    }

    if (!capability.has_value())
    {
        return std::nullopt;
    }

    const std::string model =
        PropertyOr<std::string>(
            objects,
            capability->id,
            kCapabilityModel,
            std::string{"Point Mass"});

    if (model != "Point Mass")
    {
        throw std::runtime_error(
            "Unsupported gravity capability model: " +
            model);
    }

    const bool deriveFromMass =
        PropertyOr<bool>(
            objects,
            capability->id,
            kGravityDeriveMuFromMass,
            true);

    const f64 mu =
        deriveFromMass
            ? celestial_gravity::
                GravitationalParameterFromMass(
                    bodyMassKilograms)
            : PropertyOr<f64>(
                objects,
                capability->id,
                kGravityMuM3PerS2,
                0.0);

    return GravityConfiguration{
        .gravitationalParameterM3PerS2 = mu,
        .softeningMeters =
            PropertyOr<f64>(
                objects,
                capability->id,
                kGravitySofteningMeters,
                0.0)
    };
}

[[nodiscard]] celestial_gravity::GravitySourceId
GravitySourceForBodyObject(
    const scene::ObjectId object) noexcept
{
    celestial_gravity::GravitySourceId result{
        .high =
            object.high ^
            0x4752415649545953ULL,
        .low =
            object.low ^
            0x4f52424954563036ULL
    };

    if (!result)
    {
        result.low = 1;
    }

    return result;
}

[[nodiscard]] std::optional<scene::ObjectRecord>
FindRotationCapability(
    const scene::ObjectStore& objects,
    const scene::ObjectId body)
{
    std::optional<scene::ObjectRecord> found;

    for (const auto& child : objects.Children(body))
    {
        if (child.type != kRotationCapabilityType)
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
                "Celestial body has multiple enabled rotation capabilities.");
        }

        found = child;
    }

    return found;
}

[[nodiscard]] std::shared_ptr<
    const celestial_rotation::OrientationProvider>
OrientationProviderFor(
    const scene::ObjectStore& objects,
    const scene::ObjectId body,
    const time::SimulationTime systemEpoch,
    std::shared_ptr<
        const celestial_orbits::OrbitStateProvider> orbitState)
{
    const f64 degreesToRadians =
        std::numbers::pi_v<f64> / 180.0;

    const auto rotationCapability =
        FindRotationCapability(objects, body);

    if (!rotationCapability.has_value())
    {
        const f64 periodSeconds =
            PropertyOr<f64>(
                objects,
                body,
                kBodyRotationPeriodSeconds,
                86'400.0);
        const f64 tiltRadians =
            PropertyOr<f64>(
                objects,
                body,
                kBodyAxialTiltDegrees,
                0.0) *
            degreesToRadians;
        const f64 phaseRadians =
            PropertyOr<f64>(
                objects,
                body,
                kBodyRotationPhaseDegrees,
                0.0) *
            degreesToRadians;

        return std::make_shared<
            celestial_rotation::UniformSpinOrientationProvider>(
                celestial_rotation::UniformSpinOrientation{
                    .axisInParent = {
                        0.0,
                        std::sin(tiltRadians),
                        std::cos(tiltRadians)
                    },
                    .angularVelocityRadiansPerSecond =
                        periodSeconds > 0.0
                            ? (2.0 * std::numbers::pi_v<f64>) /
                                periodSeconds
                            : 0.0,
                    .phaseRadiansAtEpoch = phaseRadians,
                    .epoch = systemEpoch
                });
    }

    const std::string model =
        PropertyOr<std::string>(
            objects,
            rotationCapability->id,
            kCapabilityModel,
            std::string{"Uniform Spin"});

    const math::Double3 axis =
        PropertyOr<math::Double3>(
            objects,
            rotationCapability->id,
            kRotationAxis,
            {0.0, 0.0, 1.0});

    if (model == "Fixed")
    {
        return std::make_shared<
            celestial_rotation::FixedOrientationProvider>(
                celestial_rotation::FixedOrientation{});
    }

    if (model == "Uniform Spin")
    {
        const f64 periodSeconds =
            PropertyOr<f64>(
                objects,
                rotationCapability->id,
                kRotationPeriodSeconds,
                86'400.0);
        const i64 epochMicroseconds =
            PropertyOr<i64>(
                objects,
                rotationCapability->id,
                kRotationEpochMicroseconds,
                systemEpoch.microsecondsFromEpoch);

        return std::make_shared<
            celestial_rotation::UniformSpinOrientationProvider>(
                celestial_rotation::UniformSpinOrientation{
                    .axisInParent = axis,
                    .angularVelocityRadiansPerSecond =
                        periodSeconds > 0.0
                            ? (2.0 * std::numbers::pi_v<f64>) /
                                periodSeconds
                            : 0.0,
                    .phaseRadiansAtEpoch =
                        PropertyOr<f64>(
                            objects,
                            rotationCapability->id,
                            kRotationPhaseDegrees,
                            0.0) *
                        degreesToRadians,
                    .epoch = {
                        .microsecondsFromEpoch =
                            epochMicroseconds
                    }
                });
    }

    if (model == "Synchronous")
    {
        return std::make_shared<
            celestial_rotation::SynchronousOrientationProvider>(
                celestial_rotation::SynchronousOrientation{
                    .orbitState = std::move(orbitState),
                    .poleInParent = axis,
                    .phaseOffsetRadians =
                        PropertyOr<f64>(
                            objects,
                            rotationCapability->id,
                            kRotationSynchronousPhaseOffsetDegrees,
                            0.0) *
                        degreesToRadians
                });
    }

    throw std::runtime_error(
        "Unsupported rotation capability model: " +
        model);
}

[[nodiscard]] universe::BodyTransformModel
TransformFor(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const time::SimulationTime epoch,
    std::shared_ptr<
        const celestial_orbits::OrbitStateProvider>
        orbitOverride = {})
{
    auto orbitState =
        orbitOverride
            ? std::move(orbitOverride)
            : OrbitProviderFor(
                objects,
                object,
                epoch);

    auto orientation =
        OrientationProviderFor(
            objects,
            object,
            epoch,
            orbitState);

    return universe::ProviderDrivenBodyTransform{
        .orbitState = std::move(orbitState),
        .orientation = std::move(orientation)
    };
}
} // namespace

UniverseComposition::UniverseComposition()
    : frames_(
          std::make_unique<frames::FrameGraph>()),
      bodies_(
          std::make_unique<universe::BodyRegistry>(
              *frames_)),
      gravity_(
          std::make_unique<
              celestial_gravity::GravityService>(
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
    auto candidateGravity =
        std::make_unique<
            celestial_gravity::GravityService>(
                *candidateFrames);

    std::unordered_map<scene::ObjectId, universe::SystemId>
        candidateSystems;
    std::unordered_map<scene::ObjectId, universe::BodyId>
        candidateBodyIds;
    std::unordered_map<scene::ObjectId, frames::FrameId>
        candidateFrameIds;
    std::unordered_map<
        scene::ObjectId,
        celestial_gravity::GravitySourceId>
        candidateGravitySources;
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
            frames::FrameId,
            bool,
            std::shared_ptr<
                const celestial_orbits::OrbitStateProvider>)>
            composeNode;

        std::function<void(
            const scene::ObjectRecord&,
            frames::FrameId,
            bool)>
            composeChildren;

        composeChildren =
            [&](const scene::ObjectRecord& parentObject,
                const frames::FrameId parentFrame,
                const bool parentFrameInertial)
            {
                const auto children =
                    objects.Children(parentObject.id);

                std::unordered_map<
                    scene::ObjectId,
                    std::shared_ptr<
                        const celestial_orbits::OrbitStateProvider>>
                    promotedProviders;

                std::vector<
                    celestial_orbits::NBodyMemberSeed>
                    seeds;

                std::optional<DynamicPromotionSettings>
                    sharedSettings;

                for (const auto& child : children)
                {
                    if (child.type !=
                        kCelestialBodyType)
                    {
                        continue;
                    }

                    const auto promotion =
                        DynamicPromotionSettingsFor(
                            objects,
                            child.id);

                    if (!promotion.has_value())
                    {
                        continue;
                    }

                    if (!parentFrameInertial)
                    {
                        throw std::runtime_error(
                            "Dynamic N-body promotion requires an inertial system/reference frame and cannot run below a rotating body-fixed frame.");
                    }

                    if (!sharedSettings.has_value())
                    {
                        sharedSettings =
                            *promotion;
                    }
                    else if (
                        sharedSettings->stepSeconds !=
                            promotion->stepSeconds ||
                        sharedSettings->softeningMeters !=
                            promotion->softeningMeters)
                    {
                        throw std::runtime_error(
                            "Sibling N-body promoted bodies must use identical step and softening settings.");
                    }

                    seeds.push_back({
                        .id =
                            NBodyMemberForObject(
                                child.id),
                        .massKilograms =
                            PropertyOr<f64>(
                                objects,
                                child.id,
                                kBodyMass,
                                5.0e24),
                        .sourceProvider =
                            OrbitProviderFor(
                                objects,
                                child.id,
                                epoch)
                    });
                }

                if (!seeds.empty())
                {
                    if (seeds.size() < 2U)
                    {
                        throw std::runtime_error(
                            "Dynamic N-body promotion requires at least two promoted sibling bodies in the same inertial/reference frame.");
                    }

                    auto domain =
                        std::make_shared<
                            celestial_orbits::NBodyDomain>(
                                epoch,
                                std::move(seeds),
                                celestial_orbits::
                                    NBodyIntegrationSettings{
                                        .stepSeconds =
                                            sharedSettings->
                                                stepSeconds,
                                        .softeningMeters =
                                            sharedSettings->
                                                softeningMeters
                                    });

                    for (const auto& child : children)
                    {
                        if (child.type !=
                            kCelestialBodyType)
                        {
                            continue;
                        }

                        if (!DynamicPromotionSettingsFor(
                                objects,
                                child.id).
                                has_value())
                        {
                            continue;
                        }

                        promotedProviders.emplace(
                            child.id,
                            domain->ProviderFor(
                                NBodyMemberForObject(
                                    child.id)));
                    }
                }

                for (const auto& child : children)
                {
                    if (child.type !=
                            kCelestialBodyType &&
                        child.type !=
                            kCelestialReferenceNodeType)
                    {
                        continue;
                    }

                    const auto promoted =
                        promotedProviders.find(
                            child.id);

                    composeNode(
                        child,
                        parentFrame,
                        parentFrameInertial,
                        promoted ==
                                promotedProviders.end()
                            ? nullptr
                            : promoted->second);
                }
            };

        composeNode =
            [&](const scene::ObjectRecord& object,
                const frames::FrameId parentFrame,
                const bool parentFrameInertial,
                std::shared_ptr<
                    const celestial_orbits::OrbitStateProvider>
                    orbitOverride)
            {
                frames::FrameId childParentFrame =
                    parentFrame;
                bool childFrameInertial =
                    parentFrameInertial;

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
                    const frames::FrameId centerFrame =
                        DerivedId<frames::FrameId>(
                            object.id,
                            0x424f445943454e54ULL,
                            0x4f52424954563036ULL);

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
                                    epoch,
                                    std::move(
                                        orbitOverride)),
                            .id = bodyId,
                            .frame = bodyFrame,
                            .centerFrame = centerFrame
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

                    if (const auto gravity =
                            GravityConfigurationFor(
                                objects,
                                object.id,
                                massKilograms);
                        gravity.has_value())
                    {
                        const auto sourceId =
                            GravitySourceForBodyObject(
                                object.id);

                        candidateGravity->RegisterSource({
                            .id = sourceId,
                            .frame = bodyFrame,
                            .model = std::make_shared<
                                celestial_gravity::
                                    PointMassGravityModel>(
                                        gravity->
                                            gravitationalParameterM3PerS2,
                                        gravity->
                                            softeningMeters)
                        });

                        candidateGravitySources.emplace(
                            object.id,
                            sourceId);
                    }

                    // Structural celestial children orbit the body center,
                    // never the rotating surface/body-fixed frame.
                    childParentFrame =
                        centerFrame;
                    // A translating body-centered frame is still not a valid
                    // inertial domain for M08 N-body promotion.
                    childFrameInertial = false;
                    ++bodyCount;
                }
                else
                {
                    return;
                }

                composeChildren(
                    object,
                    childParentFrame,
                    childFrameInertial);
            };

        composeChildren(
            systemObject,
            systemFrame,
            true);
    }

    frames_ = std::move(candidateFrames);
    bodies_ = std::move(candidateBodies);
    gravity_ = std::move(candidateGravity);
    systemByObject_ =
        std::move(candidateSystems);
    bodyByObject_ =
        std::move(candidateBodyIds);
    frameByObject_ =
        std::move(candidateFrameIds);
    gravitySourceByObject_ =
        std::move(candidateGravitySources);
    objectByBody_ =
        std::move(candidateObjects);
    sourceRevision_ = objects.Revision();

    return {
        .systems =
            static_cast<u32>(systemByObject_.size()),
        .referenceNodes = referenceNodeCount,
        .bodies = bodyCount,
        .gravitySources =
            static_cast<u32>(
                gravitySourceByObject_.size()),
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

celestial_gravity::GravityService&
UniverseComposition::Gravity() noexcept
{
    return *gravity_;
}

const celestial_gravity::GravityService&
UniverseComposition::Gravity() const noexcept
{
    return *gravity_;
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

std::optional<
    celestial_gravity::GravitySourceId>
UniverseComposition::GravitySourceForObject(
    const scene::ObjectId object) const noexcept
{
    const auto found =
        gravitySourceByObject_.find(object);

    return found ==
            gravitySourceByObject_.end()
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
