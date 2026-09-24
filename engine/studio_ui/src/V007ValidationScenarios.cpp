#include <orbit/studio_ui/V007ValidationScenarios.hpp>

#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/lighting/LightingScheduler.hpp>
#include <orbit/studio_ui/LightingInteractionState.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/volume_representation/VolumeCache.hpp>
#include <orbit/volume_representation/VolumeRepresentation.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>
#include <orbit/world_model/WorldSchemas.hpp>

#include <array>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
[[nodiscard]] std::optional<scene::ObjectId> SelectedObject(
    editor_session::EditorWorldSession& world)
{
    const auto& selected = world.Selection().Ordered();
    return selected.size() == 1U
        ? std::optional(selected.front())
        : std::nullopt;
}

void SelectSingle(
    editor_session::EditorWorldSession& world,
    const scene::ObjectId object)
{
    const std::array selection{object};
    world.Selection().Set(selection);
}

[[nodiscard]] std::optional<scene::ObjectId> CreateVolumePreset(
    editor_session::EditorWorldSession& world,
    const std::string_view preset)
{
    commands::CommandArguments args;
    args.emplace("preset", std::string(preset));
    world.CommandRegistry().Invoke(
        editor_model::authoring_commands::kCreateVolume,
        args);

    const auto selected = SelectedObject(world);
    if (!selected.has_value())
    {
        return std::nullopt;
    }

    const auto record = world.Objects().Find(*selected);
    if (!record.has_value() || record->type != world_model::kVolumeType)
    {
        return std::nullopt;
    }
    return record->id;
}

void AddVolumeInput(
    editor_session::EditorWorldSession& world,
    const commands::CommandId command,
    const std::string_view kind)
{
    commands::CommandArguments args;
    args.emplace("kind", std::string(kind));
    world.CommandRegistry().Invoke(command, args);
}

[[nodiscard]] std::optional<scene::ObjectId> ValidationParent(
    editor_session::EditorWorldSession& world)
{
    const auto selected = SelectedObject(world);
    if (!selected.has_value())
    {
        return std::nullopt;
    }

    const auto record = world.Objects().Find(*selected);
    if (!record.has_value())
    {
        return std::nullopt;
    }

    if (record->type == world_model::kWorldType ||
        record->type == world_model::kCelestialBodyType)
    {
        return record->id;
    }

    return record->parent;
}

void CreateHeadlightRig(editor_session::EditorWorldSession& world)
{
    auto& commands = world.Commands();
    const auto parent = ValidationParent(world);

    const bool ownsTransaction = !commands.HasActiveTransaction();
    if (ownsTransaction)
    {
        commands.BeginTransaction("M43 Headlight / Brake Light Validation Rig");
    }

    try
    {
        const auto left = commands.CreateObject(
            world_model::kSpotLightType,
            "M43 Headlight Left",
            parent);
        const auto right = commands.CreateObject(
            world_model::kSpotLightType,
            "M43 Headlight Right",
            parent);
        const auto brake = commands.CreateObject(
            world_model::kPointLightType,
            "M43 Brake Light Reference",
            parent);

        const auto configureSpot = [&commands](
            const scene::ObjectId id,
            const f64 x)
        {
            commands.SetProperty(id, world_model::kLightEnabled, true);
            commands.SetProperty(id, world_model::kLightPositionMeters, math::Double3{x,1.0,0.0});
            commands.SetProperty(id, world_model::kLightDirection, math::Double3{0.0,-0.08,1.0});
            commands.SetProperty(id, world_model::kLightColorLinear, math::Double3{1.0,0.91,0.78});
            commands.SetProperty(id, world_model::kLightIntensityLumens, 1500.0);
            commands.SetProperty(id, world_model::kLightRangeMeters, 80.0);
            commands.SetProperty(id, world_model::kLightInnerConeDegrees, 8.0);
            commands.SetProperty(id, world_model::kLightOuterConeDegrees, 18.0);
        };

        configureSpot(left, -0.75);
        configureSpot(right, 0.75);

        commands.SetProperty(brake, world_model::kLightEnabled, true);
        commands.SetProperty(brake, world_model::kLightPositionMeters, math::Double3{0.0,0.8,-2.0});
        commands.SetProperty(brake, world_model::kLightColorLinear, math::Double3{1.0,0.015,0.005});
        commands.SetProperty(brake, world_model::kLightIntensityLumens, 220.0);
        commands.SetProperty(brake, world_model::kLightRangeMeters, 12.0);

        if (ownsTransaction)
        {
            commands.CommitTransaction();
        }
    }
    catch (...)
    {
        if (ownsTransaction && commands.HasActiveTransaction())
        {
            commands.RollbackTransaction();
        }
        throw;
    }
}

void EnableLightingValidationOverlays()
{
    auto& overlays = StudioLightingOverlays();
    overlays.giUpdateCells = true;
    overlays.radianceCacheRegions = true;
    overlays.emissiveInfluence = true;
}

[[nodiscard]] post_process::HumanEyeAdaptationConfig EyeConfig(
    StudioViewportRenderer& renderer)
{
    const auto diagnostics =
        renderer.LuminanceHistogramDiagnostics("studio.primary");
    return diagnostics.has_value()
        ? diagnostics->eyeConfig
        : post_process::HumanEyeAdaptationConfig{};
}

[[nodiscard]] u64 CacheableFieldMask(const u64 fieldMask) noexcept
{
    const u64 density = static_cast<u64>(world_model::VolumeField::Density);
    const u64 emission = static_cast<u64>(world_model::VolumeField::Emission);
    const u64 supported = fieldMask & (density | emission);
    return supported != 0U ? supported : density;
}
} // namespace

std::string PrepareV007ValidationScenario(
    const V007ValidationScenario scenario,
    studio_session::StudioSession& session,
    StudioViewportRenderer& renderer)
{
    if (!session.World().HasWorld())
    {
        return "Open a world before preparing an M43 validation scenario.";
    }

    auto& world = session.World();

    try
    {
        switch (scenario)
        {
        case V007ValidationScenario::LedRoom:
            EnableLightingValidationOverlays();
            renderer.ResetHumanEyeAdaptation("studio.primary");
            return "LED Room inspection prepared. Use an authored emissive screen/material; M43 deliberately does not replace it with proxy lights.";

        case V007ValidationScenario::CloudGlare:
        {
            auto config = EyeConfig(renderer);
            config.photopicCeilingLog2 = 2.0F;
            config.photopicCeilingRecoverySeconds = 0.12F;
            config.overloadRecoverySeconds = 0.10F;
            renderer.SetHumanEyeAdaptationConfig("studio.primary", config);
            renderer.ResetHumanEyeAdaptation("studio.primary");
            return "Cloud Glare eye-response policy prepared; use the production atmosphere/cloud lighting scene for GPU tolerance capture.";
        }

        case V007ValidationScenario::DarkInteriorToDaylight:
        {
            auto config = EyeConfig(renderer);
            config.darkAdaptSeconds = 18.0F;
            config.darkResetSeconds = 0.30F;
            config.photopicBrightenSeconds = 0.18F;
            renderer.SetHumanEyeAdaptationConfig("studio.primary", config);
            renderer.ResetHumanEyeAdaptation("studio.primary");
            return "Dark Interior -> Daylight eye-response state reset with production adaptation constants.";
        }

        case V007ValidationScenario::HeadlightBrakeLight:
            CreateHeadlightRig(world);
            EnableLightingValidationOverlays();
            return "Undoable production point/spot validation rig created. Emissive material sampling remains a separate authority and is checked numerically.";

        case V007ValidationScenario::CityNightFlight:
            EnableLightingValidationOverlays();
            return "City Night Flight diagnostics prepared. Existing authored city emission remains the authority; cache/emissive regions are visible.";

        case V007ValidationScenario::GroundToOrbit:
            EnableLightingValidationOverlays();
            return "Ground -> Orbit diagnostics prepared on the current production world; representation transitions remain untouched.";

        case V007ValidationScenario::RtAb:
        {
            auto config = lighting::StudioLightingRuntimeConfig().value_or(
                lighting::LightingSchedulerConfig{});
            config.hardwareRayQueryEnabled = false;
            lighting::SetStudioLightingRuntimeConfig(config);
            return "RT A/B prepared at RT-off baseline. Re-enable Hardware Ray Query in Display Diagnostics without changing budgets for the B capture.";
        }

        case V007ValidationScenario::SmokeObstacleAdvection:
        {
            const auto volume = CreateVolumePreset(world, "Smoke");
            if (!volume.has_value()) return "Smoke preset creation did not yield a selected Volume.";
            AddVolumeInput(world, editor_model::authoring_commands::kAddVolumeSource, "Brush");
            SelectSingle(world, *volume);
            AddVolumeInput(world, editor_model::authoring_commands::kAddVolumeEffector, "Obstacle");
            SelectSingle(world, *volume);
            return "Smoke Volume + Brush source + Obstacle effector created through production authoring commands.";
        }

        case V007ValidationScenario::SurfaceDustWind:
        {
            const auto volume = CreateVolumePreset(world, "Dust");
            if (!volume.has_value()) return "Dust preset creation did not yield a selected Volume.";
            AddVolumeInput(world, editor_model::authoring_commands::kAddVolumeSource, "Terrain");
            SelectSingle(world, *volume);
            AddVolumeInput(world, editor_model::authoring_commands::kAddVolumeEffector, "Wind");
            SelectSingle(world, *volume);
            return "Dust Volume + Terrain source + Wind effector created through production authoring commands.";
        }

        case V007ValidationScenario::EmissiveFireGi:
        {
            const auto volume = CreateVolumePreset(world, "Fire");
            if (!volume.has_value()) return "Fire preset creation did not yield a selected Volume.";
            AddVolumeInput(world, editor_model::authoring_commands::kAddVolumeSource, "Brush");
            SelectSingle(world, *volume);
            world.Commands().SetProperty(*volume, world_model::kVolumeEmissionScale, 12.0);
            world.Commands().SetProperty(*volume, world_model::kVolumeGiEmissionScale, 1.0);
            EnableLightingValidationOverlays();
            return "Emissive Fire Volume created with production physical emission/GI authority.";
        }

        case V007ValidationScenario::RoamingDomainContinuity:
        {
            const auto volume = CreateVolumePreset(world, "Smoke");
            if (!volume.has_value()) return "Smoke preset creation did not yield a selected Volume.";
            auto& settings = renderer.VolumeRenderSettings(*volume);
            settings.followTarget = volume_representation::FollowTarget::Camera;
            settings.showRepresentationRegions = true;
            return "Roaming smoke domain created; runtime follow target is Camera while authored center remains unchanged.";
        }

        case V007ValidationScenario::LiveToBakedEquivalence:
        {
            const auto volume = CreateVolumePreset(world, "Fire");
            if (!volume.has_value()) return "Fire preset creation did not yield a selected Volume.";
            AddVolumeInput(world, editor_model::authoring_commands::kAddVolumeSource, "Brush");
            SelectSingle(world, *volume);
            const auto domain = world_model::ResolveVolumeDomain(world.Objects(), *volume);
            if (!domain.has_value()) return "Created validation Volume could not be resolved.";
            const auto inputs = world_model::ResolveVolumeInputs(world.Objects(), *volume);
            const volume_representation::VolumeCacheBakeSettings bake{
                .resolution = 16U,
                .fieldMask = CacheableFieldMask(domain->fieldMask)
            };
            auto cache = volume_representation::BakeVolumeCache(*domain, inputs, bake);
            volume_representation::VolumeCaches().Attach(*volume, std::move(cache));
            world.Commands().SetProperty(
                *volume,
                world_model::kVolumeRepresentationMode,
                static_cast<i64>(world_model::VolumeRepresentationMode::Baked));
            return "Native 16^3 validation cache baked and attached through the production cache pipeline; Volume forced to Baked for comparison.";
        }

        case V007ValidationScenario::NearToFarVolumeLod:
        {
            const auto volume = CreateVolumePreset(world, "Smoke");
            if (!volume.has_value()) return "Smoke preset creation did not yield a selected Volume.";
            auto& settings = renderer.VolumeRenderSettings(*volume);
            settings.liveDistanceMeters = 30.0;
            settings.passiveDistanceMeters = 180.0;
            settings.liveProjectedPixels = 96.0F;
            settings.passiveProjectedPixels = 12.0F;
            settings.hysteresisFraction = 0.12F;
            settings.showRepresentationRegions = true;
            return "Near/far validation Volume created with explicit Live/Coarse/Passive thresholds and region visualization.";
        }
        }
    }
    catch (const std::exception& exception)
    {
        return std::string("M43 scenario setup failed: ") + exception.what();
    }

    return "Unknown M43 validation scenario.";
}
} // namespace orbit::studio_ui
