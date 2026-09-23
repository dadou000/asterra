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
    if (const auto policy =
            volume_representation::AllocationPolicy(
                domain.object);
        policy.has_value() &&
        !policy->denseFieldRequired)
    {
        // M36 coarse/passive/baked representations are intentionally
        // non-interactive. Their bounded procedural aggregate fields are
        // populated by the representation renderer; no M33/M34 compute work
        // is scheduled while they are active.
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
