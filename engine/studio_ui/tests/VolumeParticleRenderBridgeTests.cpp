#include <orbit/studio_ui/VolumeParticleRenderBridge.hpp>

#include <cstdlib>
#include <iostream>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::cerr << "M38 particle render bridge regression failed.\n";
        std::exit(1);
    }
}
}

int main()
{
    using namespace orbit;

    std::vector<studio_session::VolumeParticleRuntimeEvent> events;
    events.push_back({
        .sourceVolume = {.high = 1U, .low = 1U},
        .request = {
            .eventId = 3U,
            .positionMeters = {1000000001.25, -2.5, 4.0},
            .velocity = {1.0F, 2.0F, 3.0F},
            .authority = 0.25F,
            .density = 0.5F,
            .emission = 0.1F
        }});
    events.push_back({
        .sourceVolume = {.high = 1U, .low = 2U},
        .request = {
            .eventId = 2U,
            .positionMeters = {1000000002.5, 0.0, 0.0},
            .authority = 0.9F,
            .density = 0.3F,
            .emission = 2.0F
        }});
    events.push_back({
        .sourceVolume = {.high = 1U, .low = 3U},
        .request = {
            .eventId = 1U,
            .positionMeters = {1000000003.5, 0.0, 0.0},
            .authority = 0.9F,
            .density = -4.0F,
            .emission = -1.0F
        }});

    const math::Double3 origin{1000000000.0, 0.0, 0.0};
    const auto batch =
        studio_ui::BuildVolumeParticleRenderBatch(
            events,
            origin,
            2U);

    Check(batch.size() == 2U);
    // Equal-authority candidates use event id for deterministic tie-breaking.
    Check(batch[0].positionMeters.x == 3.5F);
    Check(batch[1].positionMeters.x == 2.5F);
    Check(batch[0].authority == 0.9F);
    Check(batch[1].authority == 0.9F);
    Check(batch[0].density == 0.0F);
    Check(batch[0].emission == 0.0F);
    Check(batch[1].emission == 2.0F);

    const auto full =
        studio_ui::BuildVolumeParticleRenderBatch(
            events,
            origin,
            3U);
    Check(full.size() == 3U);
    Check(full[2].positionMeters.x == 1.25F);
    Check(full[2].velocityMetersPerSecond.x == 1.0F);
    Check(full[2].velocityMetersPerSecond.y == 2.0F);
    Check(full[2].velocityMetersPerSecond.z == 3.0F);

    return 0;
}
