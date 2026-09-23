#include <orbit/volume_representation/VolumeOutputCoupling.hpp>

namespace orbit::volume_representation
{
void DispatchVolumeOutputs(
    const VolumeOutputBatch& batch,
    VolumeParticleOutputSink* particleSink,
    VolumeSurfaceOutputSink* surfaceSink)
{
    if (particleSink != nullptr &&
        !batch.particles.empty())
    {
        particleSink->SubmitParticleSpawns(
            batch.volume,
            batch.particles);
    }

    if (surfaceSink != nullptr &&
        !batch.surfaceDeposits.empty())
    {
        surfaceSink->SubmitSurfaceDeposits(
            batch.volume,
            batch.surfaceDeposits);
    }
}
} // namespace orbit::volume_representation
