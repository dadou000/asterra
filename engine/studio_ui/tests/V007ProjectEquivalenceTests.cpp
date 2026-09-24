#include <orbit/content/ContentService.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/lighting/RadianceClipmapResidency.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/studio_ui/LightingDisplaySettingsRuntime.hpp>
#include <orbit/volume_render/UniversalVolumeRenderer.hpp>
#include <orbit/volume_representation/VolumeCache.hpp>
#include <orbit/volume_representation/VolumeRepresentation.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <string>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "M45 project equivalence gate failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}

void Write(
    const std::filesystem::path& path,
    const std::string_view contents)
{
    std::filesystem::create_directories(
        path.parent_path());
    std::ofstream output(
        path,
        std::ios::binary | std::ios::trunc);
    output << contents;
    Check(static_cast<bool>(output));
}

[[nodiscard]] bool Near(
    const double left,
    const double right,
    const double epsilon = 1.0e-5)
{
    return std::abs(left - right) <= epsilon;
}
} // namespace

int main()
{
    using namespace orbit;

    const auto root =
        std::filesystem::temp_directory_path() /
        "orbit-v007-m45-project-equivalence";
    std::filesystem::remove_all(root);

    std::filesystem::path manifestPath;
    std::filesystem::path materialPath;
    std::filesystem::path cachePath;
    scene::ObjectId volumeId{};
    scene::ObjectId sourceId{};
    u64 authoredFingerprint = 0U;
    u64 cachePayloadFingerprint = 0U;
    content::MaterialEmission authoredMaterialEmission{};
    studio_ui::LightingDisplaySettings authoredDisplay{};
    lighting::EmissiveVolumeSource authoredVolumeEmission{};

    {
        auto project =
            documents::ProjectDocument::Create(
                root,
                "V0.0.7 M45 Equivalence");
        manifestPath = project.ManifestPath();

        studio_session::StudioSession session(project);
        auto& world = session.World();
        auto& commands = world.Commands();

        volumeId = commands.CreateObject(
            world_model::kVolumeType,
            "M45 Fire Volume");
        commands.SetProperty(
            volumeId,
            world_model::kVolumePreset,
            std::string{"Fire"});
        commands.SetProperty(
            volumeId,
            world_model::kVolumeCenterMeters,
            math::Double3{12.0, 3.0, -8.0});
        commands.SetProperty(
            volumeId,
            world_model::kVolumeHalfExtentsMeters,
            math::Double3{7.0, 5.0, 6.0});
        commands.SetProperty(
            volumeId,
            world_model::kVolumeSolverPolicy,
            i64{2});
        commands.SetProperty(
            volumeId,
            world_model::kVolumeRepresentationMode,
            i64{4});
        commands.SetProperty(
            volumeId,
            world_model::kVolumeFieldMask,
            i64{
                static_cast<i64>(
                    static_cast<u64>(world_model::VolumeField::Density) |
                    static_cast<u64>(world_model::VolumeField::Temperature) |
                    static_cast<u64>(world_model::VolumeField::Emission))});
        commands.SetProperty(
            volumeId,
            world_model::kVolumeResolution,
            i64{72});
        commands.SetProperty(
            volumeId,
            world_model::kVolumeExtinctionScale,
            0.73);
        commands.SetProperty(
            volumeId,
            world_model::kVolumeSingleScatteringAlbedo,
            0.82);
        commands.SetProperty(
            volumeId,
            world_model::kVolumeEmissionColor,
            math::Double3{1.0, 0.24, 0.04});
        commands.SetProperty(
            volumeId,
            world_model::kVolumeEmissionScale,
            5.25);
        commands.SetProperty(
            volumeId,
            world_model::kVolumeGiEmissionScale,
            1.35);
        commands.SetProperty(
            volumeId,
            world_model::kVolumeRenderSteps,
            i64{80});
        commands.SetProperty(
            volumeId,
            world_model::kVolumeShadowSteps,
            i64{8});
        commands.SetProperty(
            volumeId,
            world_model::kVolumeTemporalWeight,
            0.91);
        commands.SetProperty(
            volumeId,
            world_model::kVolumeOutputParticlesEnabled,
            true);
        commands.SetProperty(
            volumeId,
            world_model::kVolumeOutputParticleRate,
            96.0);

        sourceId = commands.CreateObject(
            world_model::kVolumeSourceType,
            "M45 Fire Source",
            volumeId);
        commands.SetProperty(
            sourceId,
            world_model::kVolumeChildKind,
            i64{0});
        commands.SetProperty(
            sourceId,
            world_model::kVolumeChildShape,
            i64{1});
        commands.SetProperty(
            sourceId,
            world_model::kVolumeChildPositionMeters,
            math::Double3{12.0, 0.5, -8.0});
        commands.SetProperty(
            sourceId,
            world_model::kVolumeChildRadiusMeters,
            3.25);
        commands.SetProperty(
            sourceId,
            world_model::kVolumeChildScalar,
            2.0);
        commands.SetProperty(
            sourceId,
            world_model::kVolumeChildFieldMask,
            i64{
                static_cast<i64>(
                    static_cast<u64>(world_model::VolumeField::Density) |
                    static_cast<u64>(world_model::VolumeField::Temperature) |
                    static_cast<u64>(world_model::VolumeField::Emission))});

        const auto domain =
            world_model::ResolveVolumeDomain(
                world.Objects(),
                volumeId);
        Check(domain.has_value());
        const auto inputs =
            world_model::ResolveVolumeInputs(
                world.Objects(),
                volumeId);
        Check(inputs.size() == 1U);
        Check(inputs.front().object == sourceId);

        const volume_representation::VolumeCacheBakeSettings bakeSettings{
            .resolution = 20U,
            .fieldMask =
                static_cast<u64>(world_model::VolumeField::Density) |
                static_cast<u64>(world_model::VolumeField::Emission)
        };

        auto cache =
            volume_representation::BakeVolumeCache(
                *domain,
                inputs,
                bakeSettings);
        authoredFingerprint =
            cache.descriptor.authoredFingerprint;
        cachePayloadFingerprint =
            cache.payloadFingerprint;

        cachePath =
            root / "Content" / "Volumes" /
            "m45-fire.orbitvol";
        std::filesystem::create_directories(
            cachePath.parent_path());
        std::string cacheError;
        Check(volume_representation::SaveVolumeCache(
            cachePath.string(),
            cache,
            &cacheError));
        volume_representation::VolumeCaches().Attach(
            volumeId,
            std::move(cache));

        const auto emissive =
            volume_render::BuildEmissiveVolumeSource(
                *domain,
                0.75F);
        Check(emissive.has_value());
        authoredVolumeEmission = *emissive;

        const auto pbrSource =
            root / "ExternalPbr";
        Write(pbrSource / "m45_albedo.png", "base");
        Write(pbrSource / "m45_normal.png", "normal");
        Write(pbrSource / "m45_roughness.png", "rough");
        Write(pbrSource / "m45_metallic.png", "metal");

        content::ContentService content(root);
        const auto materialId =
            content.ImportPbrSet(
                pbrSource,
                "M45 Emissive Panel");
        authoredMaterialEmission = {
            .colorLinear = {0.12, 0.42, 1.0},
            .luminanceNits = 4200.0,
            .contributesToGi = true,
            .giScale = 1.8
        };
        content.SetMaterialEmission(
            materialId,
            authoredMaterialEmission);

        const auto* material =
            content.Find(materialId);
        Check(material != nullptr);
        materialPath = material->sourcePath;

        authoredDisplay.lighting.hardwareRayQueryEnabled = true;
        authoredDisplay.lighting.emissiveGiQualityScale = 1.4F;
        authoredDisplay.lighting.budget.giMs = 2.85F;
        authoredDisplay.lighting.budget.reflectionMs = 1.15F;
        authoredDisplay.display.eye.photopicCeilingLog2 = 2.6F;
        authoredDisplay.display.eye.darkAdaptSeconds = 18.0F;
        authoredDisplay.display.highlights.bloomStrength = 0.14F;
        authoredDisplay.display.highlights.glareStrength = 0.08F;
        authoredDisplay.display.colorLut.enabled = true;
        authoredDisplay.display.colorLut.strength = 0.7F;
        authoredDisplay.display.colorLutAsset =
            "Content/Color/M45.cube";
        authoredDisplay.display.output.mode =
            post_process::OutputMode::Hdr10;
        authoredDisplay.display.output.referenceWhiteNits = 203.0F;
        authoredDisplay.display.output.requestedPeakNits = 1200.0F;
        studio_ui::SaveLightingDisplaySettings(
            root,
            authoredDisplay);

        world.Checkpoint();
        project.Save();
    }

    // Simulate a full runtime teardown. A baked asset survives on disk, but
    // the process-local attachment does not: a reopened runtime must validate
    // and attach derived data again rather than inheriting hidden state.
    volume_representation::VolumeCaches().Detach(volumeId);
    Check(
        volume_representation::VolumeCaches().Find(volumeId) ==
        nullptr);

    {
        auto reopenedProject =
            documents::ProjectDocument::Open(
                manifestPath);
        studio_session::StudioSession reopenedSession(
            reopenedProject);
        auto& world = reopenedSession.World();

        const auto volumeRecord =
            world.Objects().Find(volumeId);
        const auto sourceRecord =
            world.Objects().Find(sourceId);
        Check(volumeRecord.has_value());
        Check(sourceRecord.has_value());
        Check(sourceRecord->parent.has_value());
        Check(*sourceRecord->parent == volumeId);

        const auto domain =
            world_model::ResolveVolumeDomain(
                world.Objects(),
                volumeId);
        Check(domain.has_value());
        Check(domain->preset == "Fire");
        Check(domain->solverPolicy ==
            world_model::VolumeSolverPolicy::Local3D);
        Check(domain->representationMode ==
            world_model::VolumeRepresentationMode::Baked);
        Check(domain->resolution == 72U);
        Check(Near(domain->centerMeters.x, 12.0));
        Check(Near(domain->halfExtentsMeters.y, 5.0));
        Check(Near(domain->extinctionScale, 0.73));
        Check(Near(domain->singleScatteringAlbedo, 0.82));
        Check(Near(domain->emissionScale, 5.25));
        Check(Near(domain->giEmissionScale, 1.35));
        Check(domain->renderSteps == 80U);
        Check(domain->shadowSteps == 8U);
        Check(Near(domain->temporalWeight, 0.91));
        Check(domain->outputParticlesEnabled);
        Check(Near(domain->outputParticleRatePerSecond, 96.0));

        const auto inputs =
            world_model::ResolveVolumeInputs(
                world.Objects(),
                volumeId);
        Check(inputs.size() == 1U);
        Check(inputs.front().object == sourceId);
        Check(Near(inputs.front().radiusMeters, 3.25));
        Check(Near(inputs.front().scalarValue, 2.0));

        const auto loaded =
            volume_representation::LoadVolumeCache(
                cachePath.string());
        Check(static_cast<bool>(loaded));
        Check(loaded.cache.has_value());
        Check(
            loaded.cache->descriptor.authoredFingerprint ==
            authoredFingerprint);
        Check(
            loaded.cache->payloadFingerprint ==
            cachePayloadFingerprint);
        Check(loaded.cache->descriptor.volume == volumeId);

        const volume_representation::VolumeCacheBakeSettings reopenedBake{
            .resolution = loaded.cache->descriptor.resolutionX,
            .fieldMask = loaded.cache->descriptor.fieldMask
        };
        std::string cacheReason;
        Check(volume_representation::IsVolumeCacheCurrent(
            *loaded.cache,
            *domain,
            inputs,
            reopenedBake,
            &cacheReason));

        volume_representation::VolumeCaches().Attach(
            volumeId,
            *loaded.cache);

        volume_representation::RepresentationInput representation{};
        representation.volume = volumeId;
        representation.volumeCenterInFrameMeters =
            domain->centerMeters;
        representation.halfExtentsMeters =
            domain->halfExtentsMeters;
        representation.observerInFrameMeters =
            domain->centerMeters +
            math::Double3{0.0, 0.0, 30.0};
        representation.stableFrame = 45U;
        representation.stableBody = 7U;
        representation.viewportHeightPixels = 1080U;
        representation.verticalFovRadians = 1.0F;
        representation.authoredMode =
            domain->representationMode;
        representation.bakedAvailable = true;
        const auto resolvedRepresentation =
            volume_representation::ResolveRepresentation(
                representation,
                volume_representation::ResolvedRepresentation::Live);
        Check(resolvedRepresentation.forced);
        Check(!resolvedRepresentation.bakedFallback);
        Check(resolvedRepresentation.representation ==
            volume_representation::ResolvedRepresentation::Baked);

        const auto reopenedEmission =
            volume_render::BuildEmissiveVolumeSource(
                *domain,
                0.75F);
        Check(reopenedEmission.has_value());
        Check(Near(
            reopenedEmission->intensityScale,
            authoredVolumeEmission.intensityScale));
        Check(Near(
            reopenedEmission->colorLinear.x,
            authoredVolumeEmission.colorLinear.x));
        Check(Near(
            reopenedEmission->colorLinear.y,
            authoredVolumeEmission.colorLinear.y));
        Check(Near(
            reopenedEmission->colorLinear.z,
            authoredVolumeEmission.colorLinear.z));

        content::ContentService reopenedContent(root);
        reopenedContent.Scan();
        const auto* material =
            reopenedContent.FindByPath(materialPath);
        Check(material != nullptr);
        const auto emission =
            reopenedContent.ResolveMaterialEmission(
                material->id);
        Check(Near(
            emission.luminanceNits,
            authoredMaterialEmission.luminanceNits));
        Check(Near(
            emission.colorLinear[0],
            authoredMaterialEmission.colorLinear[0]));
        Check(Near(
            emission.colorLinear[1],
            authoredMaterialEmission.colorLinear[1]));
        Check(Near(
            emission.colorLinear[2],
            authoredMaterialEmission.colorLinear[2]));
        Check(
            emission.contributesToGi ==
            authoredMaterialEmission.contributesToGi);
        Check(Near(
            emission.giScale,
            authoredMaterialEmission.giScale));

        const auto reopenedDisplay =
            studio_ui::LoadLightingDisplaySettings(root);
        Check(
            reopenedDisplay.lighting.hardwareRayQueryEnabled ==
            authoredDisplay.lighting.hardwareRayQueryEnabled);
        Check(Near(
            reopenedDisplay.lighting.emissiveGiQualityScale,
            authoredDisplay.lighting.emissiveGiQualityScale));
        Check(Near(
            reopenedDisplay.lighting.budget.giMs,
            authoredDisplay.lighting.budget.giMs));
        Check(Near(
            reopenedDisplay.display.eye.photopicCeilingLog2,
            authoredDisplay.display.eye.photopicCeilingLog2));
        Check(Near(
            reopenedDisplay.display.highlights.bloomStrength,
            authoredDisplay.display.highlights.bloomStrength));
        Check(
            reopenedDisplay.display.colorLutAsset ==
            authoredDisplay.display.colorLutAsset);
        Check(
            reopenedDisplay.display.output.mode ==
            authoredDisplay.display.output.mode);
        Check(Near(
            reopenedDisplay.display.output.requestedPeakNits,
            authoredDisplay.display.output.requestedPeakNits));

        // Radiance cache residency is runtime-derived. A fresh instance has no
        // inherited radiance values; the first production frame repopulates it
        // from the same reopened semantic/material/volume authority.
        lighting::RadianceClipmapResidency radiance(
            lighting::RadianceClipmapConfig{});
        const auto freshStats = radiance.Stats();
        Check(freshStats.residentCells == 0U);
        Check(freshStats.dirtyCells == 0U);
    }

    volume_representation::VolumeCaches().Detach(volumeId);
    std::filesystem::remove_all(root);

    std::cout
        << "M45 save/reopen project equivalence gate passed.\n";
    return 0;
}
