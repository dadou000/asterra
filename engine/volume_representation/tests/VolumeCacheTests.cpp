#include <orbit/volume_representation/VolumeCache.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <vector>

namespace
{
void CheckCache(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "Volume cache test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}
} // namespace

void RunVolumeCacheTests()
{
    using namespace orbit;
    using namespace orbit::volume_representation;

    world_model::ResolvedVolumeDomain domain;
    domain.object = {.high = 0x37U, .low = 0x01U};
    domain.preset = "Fire";
    domain.centerMeters = {12.0, 4.0, -8.0};
    domain.halfExtentsMeters = {5.0, 3.0, 4.0};
    domain.fieldMask =
        static_cast<u64>(world_model::VolumeField::Density) |
        static_cast<u64>(world_model::VolumeField::Emission);

    std::vector<world_model::ResolvedVolumeInput> inputs;
    inputs.push_back({
        .object = {.high = 0x37U, .low = 0x02U},
        .role = world_model::VolumeInputRole::Source,
        .enabled = true,
        .order = 0,
        .kind = static_cast<i64>(world_model::VolumeSourceKind::Brush),
        .shape = world_model::VolumeSourceShape::Sphere,
        .positionMeters = domain.centerMeters,
        .radiusMeters = 2.0,
        .halfExtentsMeters = {2.0,2.0,2.0},
        .scalarValue = 0.4,
        .fieldMask =
            static_cast<u64>(world_model::VolumeField::Density) |
            static_cast<u64>(world_model::VolumeField::Emission),
        .fingerprint = 0x12345678U
    });

    const VolumeCacheBakeSettings settings{
        .resolution = 8U,
        .fieldMask = domain.fieldMask
    };

    const auto first = BakeVolumeCache(domain, inputs, settings);
    const auto second = BakeVolumeCache(domain, inputs, settings);

    CheckCache(first.descriptor.schemaVersion == kVolumeCacheSchemaVersion);
    CheckCache(first.descriptor.resolutionX == 8U);
    CheckCache(first.density.size() == 512U);
    CheckCache(first.emission.size() == 512U);
    CheckCache(first.descriptor.authoredFingerprint ==
        second.descriptor.authoredFingerprint);
    CheckCache(first.payloadFingerprint == second.payloadFingerprint);
    CheckCache(SampleVolumeCacheDensity(first, 0.5, 0.5, 0.5) > 0.0F);
    CheckCache(SampleVolumeCacheEmission(first, 0.5, 0.5, 0.5) > 0.0F);

    std::string reason;
    CheckCache(IsVolumeCacheCurrent(first, domain, inputs, settings, &reason));

    auto changedDomain = domain;
    changedDomain.preset = "Smoke";
    CheckCache(!IsVolumeCacheCurrent(
        first,
        changedDomain,
        inputs,
        settings,
        &reason));

    const auto path =
        std::filesystem::temp_directory_path() /
        "orbit_m37_volume_cache_test.orbitvol";

    std::string error;
    CheckCache(SaveVolumeCache(path.string(), first, &error));
    const auto loaded = LoadVolumeCache(path.string());
    CheckCache(static_cast<bool>(loaded));
    CheckCache(loaded.cache->payloadFingerprint == first.payloadFingerprint);
    CheckCache(loaded.cache->density == first.density);
    CheckCache(loaded.cache->emission == first.emission);

    VolumeCaches().Attach(domain.object, *loaded.cache);
    CheckCache(VolumeCaches().Has(domain.object));
    CheckCache(VolumeCaches().Find(domain.object) != nullptr);
    VolumeCaches().Detach(domain.object);
    CheckCache(!VolumeCaches().Has(domain.object));

    // The payload checksum must reject damaged cache data rather than allowing
    // a Baked representation to render corrupted values.
    {
        std::fstream corrupt(path, std::ios::binary | std::ios::in | std::ios::out);
        corrupt.seekg(-1, std::ios::end);
        char byte = 0;
        corrupt.read(&byte, 1);
        byte ^= static_cast<char>(0x5a);
        corrupt.seekp(-1, std::ios::end);
        corrupt.write(&byte, 1);
    }

    const auto damaged = LoadVolumeCache(path.string());
    CheckCache(!static_cast<bool>(damaged));
    CheckCache(damaged.status == VolumeCacheLoadStatus::CorruptPayload);

    std::error_code ec;
    std::filesystem::remove(path, ec);

    // A cache filled by a simulation (not baked) with unequal axis resolutions
    // becomes valid for Save/Load once FinalizeVolumeCache stamps its payload.
    {
        VolumeCacheData sim;
        sim.descriptor.resolutionX = 4U;
        sim.descriptor.resolutionY = 3U;
        sim.descriptor.resolutionZ = 2U;
        sim.descriptor.fieldMask = static_cast<u64>(world_model::VolumeField::Density);
        sim.descriptor.centerMeters = {0.0, 0.0, 0.0};
        sim.descriptor.halfExtentsMeters = {2.0, 1.5, 1.0};
        sim.density.assign(24U, 0.0F);
        sim.density[(1U * 3U + 2U) * 4U + 3U] = 0.75F; // x=3 y=2 z=1
        FinalizeVolumeCache(sim);
        CheckCache(sim.payloadFingerprint != 0U);
        // The far corner samples exactly the written cell; the origin is clear.
        CheckCache(SampleVolumeCacheDensity(sim, 1.0, 1.0, 1.0) == 0.75F);
        CheckCache(SampleVolumeCacheDensity(sim, 0.0, 0.0, 0.0) == 0.0F);

        const auto simPath = std::filesystem::temp_directory_path() /
            "orbit_simulated_volume_cache_test.orbitvol";
        std::string simError;
        CheckCache(SaveVolumeCache(simPath.string(), sim, &simError));
        const auto reloaded = LoadVolumeCache(simPath.string());
        CheckCache(static_cast<bool>(reloaded));
        CheckCache(reloaded.cache->density == sim.density);
        CheckCache(reloaded.cache->descriptor.resolutionZ == 2U);
        std::error_code simEc;
        std::filesystem::remove(simPath, simEc);
    }
}
