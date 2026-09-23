#include <orbit/volume_solver/SurfaceVolumeSolver.hpp>
#include <orbit/volume_representation/VolumeRepresentation.hpp>

namespace orbit::volume_solver
{
void SurfaceVolumeSolverService::AddPasses(
    render_graph::RenderGraph& graph,
    const std::string_view prefix,
    const scene::ObjectStore& objects,
    const world_model::ResolvedVolumeDomain& domain,
    volume_fields::VolumeFieldStorage& storage,
    const volume_fields::ImportedVolumeFields& fields,
    const u32 frameSlot)
{
    const auto policy =
        volume_representation::AllocationPolicy(
            domain.object);

    const bool authoredFarForce =
        domain.representationMode ==
            world_model::VolumeRepresentationMode::Coarse ||
        domain.representationMode ==
            world_model::VolumeRepresentationMode::Passive ||
        domain.representationMode ==
            world_model::VolumeRepresentationMode::Baked;

    if ((policy.has_value() &&
         !policy->denseFieldRequired) ||
        (!policy.has_value() &&
         authoredFarForce))
    {
        // M36 coarse/passive/baked representations are intentionally
        // non-interactive. Their bounded procedural aggregate fields are
        // populated by the representation renderer; no M33/M34 compute work
        // is scheduled while they are active. The authored-mode fallback also
        // closes the first-frame gap before a viewport has published policy.
        return;
    }

    AddPassesBase(
        graph,
        prefix,
        objects,
        domain,
        storage,
        fields,
        frameSlot);
}
} // namespace orbit::volume_solver
