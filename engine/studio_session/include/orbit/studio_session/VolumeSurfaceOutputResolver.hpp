#pragma once

#include <orbit/frames/FrameGraph.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/surface_model/SurfaceComposition.hpp>
#include <orbit/universe/ReferenceSurface.hpp>
#include <orbit/volume_representation/VolumeOutputRuntime.hpp>
#include <orbit/world_model/UniverseComposition.hpp>

#include <span>
#include <vector>

namespace orbit::studio_session
{
struct ResolvedVolumeSurfaceDeposit
{
    scene::ObjectId volume{};
    universe::BodyId body{};
    frames::FrameId frame{};
    universe::SurfaceCoordinate coordinate{};
    math::Double3 bodyLocalSurfacePointMeters{};
    volume_representation::VolumeSurfaceDepositRequest request{};
};

struct VolumeSurfaceOutputResolverDiagnostics
{
    u32 submitted{0U};
    u32 resolved{0U};
    u32 missingOwningBody{0U};
    u32 missingRuntimeBody{0U};
    u32 missingSurfaceCapability{0U};
    u32 invalidProjectionDirection{0U};
    u32 projectionMiss{0U};
    u32 projectionOutOfRange{0U};
    u32 coordinateFailure{0U};
};

// Resolves M38 transport-neutral surface requests against the authoritative
// composed world. Volume coordinates are interpreted in the owning body's
// body-fixed frame, matching Orbit's body-local surface/terrain semantics.
// Projection is radial with respect to the body shape, never a global -Y ray.
class VolumeSurfaceOutputResolver
{
public:
    void Resolve(
        const scene::ObjectStore& objects,
        const world_model::UniverseComposition& universe,
        const surface_model::SurfaceComposition& surfaces,
        std::span<const volume_representation::VolumeSurfaceQueuedRequest>
            requests);

    [[nodiscard]] std::span<const ResolvedVolumeSurfaceDeposit>
    Resolved() const noexcept;

    [[nodiscard]] const VolumeSurfaceOutputResolverDiagnostics&
    Diagnostics() const noexcept;

    void Clear() noexcept;

private:
    std::vector<ResolvedVolumeSurfaceDeposit> resolved_;
    VolumeSurfaceOutputResolverDiagnostics diagnostics_{};
};
} // namespace orbit::studio_session
