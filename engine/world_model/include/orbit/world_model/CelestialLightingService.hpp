#pragma once

#include <orbit/celestial_lighting/CelestialLighting.hpp>
#include <orbit/time/SimulationTime.hpp>
#include <orbit/universe/BodyRegistry.hpp>
#include <orbit/world_model/UniverseComposition.hpp>

#include <optional>
#include <vector>

namespace orbit::world_model
{
struct DirectBodyLighting
{
    universe::BodyId receiver{};
    universe::BodyId emitter{};
    math::Double3 receiverToEmitterMeters{};
    math::Double3 receiverBodyFixedToEmitterMeters{};
    f64 sourceDistanceMeters{0.0};
    f64 visibleFraction{1.0};
    f64 irradianceWattsPerSquareMeter{0.0};
    std::vector<
        universe::BodyId>
        contributingOccluders;
};

struct ReflectedBodyLighting
{
    universe::BodyId observer{};
    universe::BodyId reflector{};
    universe::BodyId emitter{};
    f64 phaseAngleRadians{0.0};
    f64 phaseFunction{0.0};
    f64 incidentIrradianceWattsPerSquareMeter{0.0};
    f64 unitGeometricAlbedoIrradianceWattsPerSquareMeter{0.0};
    f64 sourceVisibleFractionAtReflector{1.0};
};

class CelestialLightingService
{
public:
    CelestialLightingService(
        const scene::ObjectStore& objects,
        const UniverseComposition& universe);

    [[nodiscard]] std::optional<DirectBodyLighting>
    DirectLightingAtBody(
        universe::BodyId receiver,
        universe::BodyId emitter,
        const std::vector<universe::BodyId>& occluders,
        time::SimulationTime atTime) const;

    [[nodiscard]] std::optional<ReflectedBodyLighting>
    ReflectedLightingAtObserver(
        universe::BodyId observer,
        universe::BodyId reflector,
        universe::BodyId emitter,
        const std::vector<universe::BodyId>& occluders,
        time::SimulationTime atTime) const;

private:
    [[nodiscard]] std::optional<math::Double3>
    CenterOfBodyInFrame(
        universe::BodyId body,
        frames::FrameId targetFrame,
        time::SimulationTime atTime) const;

    const scene::ObjectStore& objects_;
    const UniverseComposition& universe_;
};
} // namespace orbit::world_model
