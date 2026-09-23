#pragma once

#include <orbit/celestial_gravity/GravityService.hpp>
#include <orbit/surface_model/SurfaceComposition.hpp>
#include <orbit/volume_representation/VolumeOutputRuntime.hpp>
#include <orbit/world_model/CelestialSchemas.hpp>
#include <orbit/world_model/UniverseComposition.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace orbit::studio_session
{
struct VolumeParticlePhysicsEnvironment
{
    f64 gravitationalParameterM3PerS2{0.0};
    f64 gravitySofteningMeters{0.0};
    math::Double3 surfaceRadiiMeters{};
    bool hasPhysicalSurface{false};
};

struct VolumeParticleRuntimeEvent
{
    scene::ObjectId sourceVolume{};
    volume_representation::VolumeParticleSpawnRequest request{};
    VolumeParticlePhysicsEnvironment physics{};
};

struct VolumeParticleOutputDiagnostics
{
    u32 submitted{0U};
    u32 retained{0U};
    u32 missingOwningBody{0U};
    u32 missingRuntimeBody{0U};
    u32 withoutGravityAuthority{0U};
    u32 withoutPhysicalSurface{0U};
    u64 generation{0U};
};

class VolumeParticleOutputState
{
public:
    void Consume(
        const scene::ObjectStore& objects,
        const world_model::UniverseComposition& universe,
        const surface_model::SurfaceComposition& surfaces,
        std::vector<volume_representation::VolumeParticleQueuedRequest> requests)
    {
        diagnostics_.submitted = static_cast<u32>(requests.size());
        diagnostics_.retained = 0U;
        diagnostics_.missingOwningBody = 0U;
        diagnostics_.missingRuntimeBody = 0U;
        diagnostics_.withoutGravityAuthority = 0U;
        diagnostics_.withoutPhysicalSurface = 0U;

        events_.clear();
        events_.reserve(requests.size());

        for (auto& queued : requests)
        {
            const auto bodyId = FindOwningBody(objects, universe, queued.volume);
            if (!bodyId.has_value())
            {
                ++diagnostics_.missingOwningBody;
                continue;
            }

            const auto* body = universe.Bodies().FindBody(*bodyId);
            if (body == nullptr)
            {
                ++diagnostics_.missingRuntimeBody;
                continue;
            }

            VolumeParticlePhysicsEnvironment physics{};
            physics.surfaceRadiiMeters = ShapeRadii(body->shape);
            physics.hasPhysicalSurface =
                surfaces.TerrainObjectForBody(*bodyId).has_value();
            if (!physics.hasPhysicalSurface)
            {
                ++diagnostics_.withoutPhysicalSurface;
            }

            const auto bodyObject = universe.ObjectForBody(*bodyId);
            if (bodyObject.has_value())
            {
                ResolveGravity(objects, *bodyObject, *body, physics);
            }
            if (!(physics.gravitationalParameterM3PerS2 > 0.0))
            {
                ++diagnostics_.withoutGravityAuthority;
            }

            events_.push_back({
                .sourceVolume = queued.volume,
                .request = std::move(queued.request),
                .physics = physics
            });
        }

        diagnostics_.retained = static_cast<u32>(events_.size());
        ++diagnostics_.generation;
    }

    // Regression/transport-only seam. Runtime code should use the composed
    // world overload above so authored gravity/surface authority is retained.
    void Consume(
        std::vector<volume_representation::VolumeParticleQueuedRequest> requests)
    {
        diagnostics_.submitted = static_cast<u32>(requests.size());
        diagnostics_.retained = 0U;
        diagnostics_.missingOwningBody = 0U;
        diagnostics_.missingRuntimeBody = 0U;
        diagnostics_.withoutGravityAuthority = 0U;
        diagnostics_.withoutPhysicalSurface = 0U;
        events_.clear();
        events_.reserve(requests.size());
        for (auto& queued : requests)
        {
            events_.push_back({
                .sourceVolume = queued.volume,
                .request = std::move(queued.request)
            });
        }
        diagnostics_.retained = static_cast<u32>(events_.size());
        ++diagnostics_.generation;
    }

    [[nodiscard]] std::span<const VolumeParticleRuntimeEvent>
    Events() const noexcept
    {
        return events_;
    }

    [[nodiscard]] const VolumeParticleOutputDiagnostics&
    Diagnostics() const noexcept
    {
        return diagnostics_;
    }

    void Clear() noexcept
    {
        events_.clear();
        diagnostics_ = {};
    }

private:
    template <typename T>
    [[nodiscard]] static T PropertyOr(
        const scene::ObjectStore& objects,
        scene::ObjectId object,
        schema::PropertyId property,
        T fallback)
    {
        const auto stored = objects.GetProperty(object, property);
        if (!stored.has_value())
        {
            return fallback;
        }
        const auto* value = std::get_if<T>(&*stored);
        return value != nullptr ? *value : fallback;
    }

    [[nodiscard]] static std::optional<universe::BodyId>
    FindOwningBody(
        const scene::ObjectStore& objects,
        const world_model::UniverseComposition& universe,
        scene::ObjectId volume)
    {
        auto current = objects.Find(volume);
        while (current.has_value())
        {
            if (const auto body = universe.BodyForObject(current->id);
                body.has_value())
            {
                return body;
            }
            if (!current->parent.has_value())
            {
                break;
            }
            current = objects.Find(*current->parent);
        }
        return std::nullopt;
    }

    [[nodiscard]] static math::Double3 ShapeRadii(
        const universe::BodyShape& shape) noexcept
    {
        return std::visit(
            [](const auto& value) -> math::Double3
            {
                using Shape = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Shape, universe::SphereShape>)
                {
                    const f64 r = std::max(value.radiusMeters, 0.0);
                    return {r, r, r};
                }
                else
                {
                    return {
                        std::max(value.radiiMeters.x, 0.0),
                        std::max(value.radiiMeters.y, 0.0),
                        std::max(value.radiiMeters.z, 0.0)
                    };
                }
            },
            shape);
    }

    static void ResolveGravity(
        const scene::ObjectStore& objects,
        scene::ObjectId bodyObject,
        const universe::CelestialBody& body,
        VolumeParticlePhysicsEnvironment& physics)
    {
        std::optional<scene::ObjectRecord> capability;
        for (const auto& child : objects.Children(bodyObject))
        {
            if (child.type != world_model::kGravityCapabilityType ||
                !PropertyOr<bool>(
                    objects,
                    child.id,
                    world_model::kCapabilityEnabled,
                    true))
            {
                continue;
            }
            if (capability.has_value())
            {
                return;
            }
            capability = child;
        }

        if (!capability.has_value())
        {
            return;
        }

        const std::string model = PropertyOr<std::string>(
            objects,
            capability->id,
            world_model::kCapabilityModel,
            std::string{"Point Mass"});
        if (model != "Point Mass")
        {
            return;
        }

        const bool deriveFromMass = PropertyOr<bool>(
            objects,
            capability->id,
            world_model::kGravityDeriveMuFromMass,
            true);

        if (deriveFromMass)
        {
            if (body.mass.has_value())
            {
                physics.gravitationalParameterM3PerS2 =
                    celestial_gravity::GravitationalParameterFromMass(
                        std::max(body.mass->massKilograms, 0.0));
            }
        }
        else
        {
            physics.gravitationalParameterM3PerS2 = std::max(
                PropertyOr<f64>(
                    objects,
                    capability->id,
                    world_model::kGravityMuM3PerS2,
                    0.0),
                0.0);
        }

        physics.gravitySofteningMeters = std::max(
            PropertyOr<f64>(
                objects,
                capability->id,
                world_model::kGravitySofteningMeters,
                0.0),
            0.0);
    }

    std::vector<VolumeParticleRuntimeEvent> events_;
    VolumeParticleOutputDiagnostics diagnostics_{};
};

[[nodiscard]] inline VolumeParticleOutputState&
VolumeParticleOutputs() noexcept
{
    static VolumeParticleOutputState state;
    return state;
}
} // namespace orbit::studio_session
