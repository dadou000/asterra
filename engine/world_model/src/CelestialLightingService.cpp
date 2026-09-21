#include <orbit/world_model/CelestialLightingService.hpp>

#include <orbit/world_model/CelestialRadiometryBinding.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::world_model
{
CelestialLightingService::CelestialLightingService(
    const scene::ObjectStore& objects,
    const UniverseComposition& universe)
    : objects_(objects),
      universe_(universe)
{
}

std::optional<math::Double3>
CelestialLightingService::CenterOfBodyInFrame(
    const universe::BodyId body,
    const frames::FrameId targetFrame,
    const time::SimulationTime atTime) const
{
    const auto* record =
        universe_.Bodies().FindBody(
            body);

    if (record == nullptr)
    {
        return std::nullopt;
    }

    const auto transformed =
        universe_.Frames().
            TransformPoint(
                frames::FramePoint{
                    .frame =
                        record->centerFrame,
                    .localMeters = {}
                },
                targetFrame,
                atTime);

    return transformed.has_value()
        ? std::optional(
              transformed->localMeters)
        : std::nullopt;
}

std::optional<DirectBodyLighting>
CelestialLightingService::DirectLightingAtBody(
    const universe::BodyId receiver,
    const universe::BodyId emitter,
    const std::vector<universe::BodyId>& occluders,
    const time::SimulationTime atTime) const
{
    if (!receiver ||
        !emitter ||
        receiver == emitter)
    {
        throw std::invalid_argument(
            "Direct body lighting requires distinct valid receiver and emitter bodies.");
    }

    const auto* receiverBody =
        universe_.Bodies().FindBody(
            receiver);
    const auto* emitterBody =
        universe_.Bodies().FindBody(
            emitter);

    if (receiverBody == nullptr ||
        emitterBody == nullptr)
    {
        return std::nullopt;
    }

    const auto emitterObject =
        universe_.ObjectForBody(
            emitter);

    if (!emitterObject.has_value())
    {
        return std::nullopt;
    }

    const auto radiative =
        ResolveRadiativeBody(
            objects_,
            *emitterObject);

    if (!radiative.has_value())
    {
        return std::nullopt;
    }

    const auto receiverToEmitter =
        CenterOfBodyInFrame(
            emitter,
            receiverBody->centerFrame,
            atTime);

    if (!receiverToEmitter.has_value())
    {
        return std::nullopt;
    }

    const f64 sourceDistance =
        math::Length(
            *receiverToEmitter);

    if (!std::isfinite(sourceDistance) ||
        sourceDistance <= 0.0)
    {
        return std::nullopt;
    }

    const celestial_lighting::ApparentDisc
        sourceDisc{
            .centerFromObserverMeters =
                *receiverToEmitter,
            .radiusMeters =
                radiative->
                    photosphereRadiusMeters
        };

    std::vector<
        celestial_lighting::ApparentDisc>
        occluderDiscs;
    std::vector<universe::BodyId>
        candidateIds;

    occluderDiscs.reserve(
        occluders.size());
    candidateIds.reserve(
        occluders.size());

    for (const auto bodyId :
         occluders)
    {
        if (!bodyId ||
            bodyId == receiver ||
            bodyId == emitter)
        {
            continue;
        }

        const auto* occluderBody =
            universe_.Bodies().FindBody(
                bodyId);

        if (occluderBody == nullptr)
        {
            continue;
        }

        const auto center =
            CenterOfBodyInFrame(
                bodyId,
                receiverBody->centerFrame,
                atTime);

        if (!center.has_value())
        {
            continue;
        }

        occluderDiscs.push_back({
            .centerFromObserverMeters =
                *center,
            .radiusMeters =
                universe::
                    ReferenceRadiusMeters(
                        occluderBody->shape)
        });
        candidateIds.push_back(
            bodyId);
    }

    const auto combined =
        celestial_lighting::
            CombinedOccultation(
                sourceDisc,
                occluderDiscs);

    std::vector<universe::BodyId>
        contributing;

    for (std::size_t index = 0;
         index < occluderDiscs.size();
         ++index)
    {
        const auto result =
            celestial_lighting::
                FiniteDiscOccultation(
                    sourceDisc,
                    occluderDiscs[index]);

        if (result.overlapping)
        {
            contributing.push_back(
                candidateIds[index]);
        }
    }

    const auto direct =
        celestial_lighting::
            AttenuatedDirectIrradiance(
                radiative->
                    radiative.
                    luminosityWatts,
                sourceDistance,
                combined.visibleFraction);

    return DirectBodyLighting{
        .receiver = receiver,
        .emitter = emitter,
        .receiverToEmitterMeters =
            *receiverToEmitter,
        .sourceDistanceMeters =
            sourceDistance,
        .visibleFraction =
            combined.visibleFraction,
        .irradianceWattsPerSquareMeter =
            direct.
                irradianceWattsPerSquareMeter,
        .contributingOccluders =
            std::move(contributing)
    };
}

std::optional<ReflectedBodyLighting>
CelestialLightingService::ReflectedLightingAtObserver(
    const universe::BodyId observer,
    const universe::BodyId reflector,
    const universe::BodyId emitter,
    const std::vector<universe::BodyId>& occluders,
    const time::SimulationTime atTime) const
{
    if (!observer ||
        !reflector ||
        !emitter ||
        observer == reflector ||
        reflector == emitter)
    {
        throw std::invalid_argument(
            "Reflected lighting requires distinct valid observer, reflector and emitter bodies.");
    }

    const auto* reflectorBody =
        universe_.Bodies().FindBody(
            reflector);
    const auto* observerBody =
        universe_.Bodies().FindBody(
            observer);

    if (reflectorBody == nullptr ||
        observerBody == nullptr)
    {
        return std::nullopt;
    }

    const auto direct =
        DirectLightingAtBody(
            reflector,
            emitter,
            occluders,
            atTime);

    if (!direct.has_value())
    {
        return std::nullopt;
    }

    const auto reflectorToObserver =
        CenterOfBodyInFrame(
            observer,
            reflectorBody->centerFrame,
            atTime);

    if (!reflectorToObserver.has_value())
    {
        return std::nullopt;
    }

    const f64 observerDistance =
        math::Length(
            *reflectorToObserver);

    if (!std::isfinite(observerDistance) ||
        observerDistance <= 0.0)
    {
        return std::nullopt;
    }

    const auto sourceDirection =
        math::Normalize(
            direct->
                receiverToEmitterMeters);
    const auto observerDirection =
        math::Normalize(
            *reflectorToObserver);

    const f64 phaseAngle =
        std::acos(
            std::clamp(
                math::Dot(
                    sourceDirection,
                    observerDirection),
                -1.0,
                1.0));

    const auto reflected =
        celestial_lighting::
            LambertSphereReflectedLight(
                direct->
                    irradianceWattsPerSquareMeter,
                universe::
                    ReferenceRadiusMeters(
                        reflectorBody->shape),
                observerDistance,
                phaseAngle);

    return ReflectedBodyLighting{
        .observer = observer,
        .reflector = reflector,
        .emitter = emitter,
        .phaseAngleRadians =
            reflected.phaseAngleRadians,
        .phaseFunction =
            reflected.phaseFunction,
        .incidentIrradianceWattsPerSquareMeter =
            reflected.
                incidentIrradianceWattsPerSquareMeter,
        .unitGeometricAlbedoIrradianceWattsPerSquareMeter =
            reflected.
                unitGeometricAlbedoIrradianceWattsPerSquareMeter,
        .sourceVisibleFractionAtReflector =
            direct->visibleFraction
    };
}
} // namespace orbit::world_model
