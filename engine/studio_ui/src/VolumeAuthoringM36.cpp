// Pre-include every dependency used by the preserved implementation before the
// method-name macros below. Include guards then prevent Register/Draw from
// leaking into dependency declarations; only VolumeAuthoringUi's two method
// definitions are renamed into the M30-M35 base implementation.
#include <orbit/studio_ui/VolumeAuthoringUi.hpp>
#include <orbit/editor_model/AuthoringCommands.hpp>
#include <orbit/studio_ui/StudioViewportRenderer.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <array>
#include <exception>
#include <format>
#include <optional>
#include <utility>

// M36 wraps the preserved M30-M35 Volumes implementation in the same
// translation unit. This keeps one panel and one authoring workflow while
// allowing the representation policy to extend it without copying thousands of
// lines of source/effect/solver/render controls.
#define Register RegisterBase
#define Draw DrawBase
#include "VolumeAuthoringUi.cpp"
#undef Draw
#undef Register

#include <orbit/volume_representation/VolumeCache.hpp>
#include <orbit/volume_representation/VolumeOutputCoupling.hpp>
#include <orbit/volume_representation/VolumeOutputRuntime.hpp>
#include <orbit/volume_representation/VolumeRepresentation.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::studio_ui
{
void VolumeAuthoringUi::Register(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanel,
        .title = "Volumes",
        .defaultOpen = false,
        .defaultDock =
            editor_ui::DockRegion::Right,
        .dockOrder = 35,
        .minSize = {
            .width = 320.0F,
            .height = 300.0F
        },
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }
    });
}

void VolumeAuthoringUi::Draw(
    editor_ui::PanelContext& context)
{
    DrawBase(context);
    DrawRepresentationPolicy(context);
}

void VolumeAuthoringUi::DrawRepresentationPolicy(
    editor_ui::PanelContext& context)
{
    if (session_ == nullptr ||
        renderer_ == nullptr ||
        !session_->World().HasWorld())
    {
        return;
    }

    auto& world =
        session_->World();
    const auto volumeId =
        SelectedVolumeId(world);

    if (!volumeId.has_value())
    {
        return;
    }

    const auto volume =
        world_model::ResolveVolumeDomain(
            world.Objects(),
            *volumeId);

    if (!volume.has_value())
    {
        return;
    }

    auto& settings =
        renderer_->VolumeRenderSettings(
            *volumeId);
    const auto diagnostics =
        renderer_->VolumeRenderDiagnostics(
            *volumeId);

    context.Separator();
    context.Heading("Representation / LOD");
    context.MutedText(
        "Auto promotes near/important effects to Live, reduces medium effects to Coarse, and uses bounded passive representation at distance.");

    context.Text("Force Representation");

    constexpr std::array modes{
        std::pair{
            "Auto",
            world_model::VolumeRepresentationMode::Auto},
        std::pair{
            "Live",
            world_model::VolumeRepresentationMode::Live},
        std::pair{
            "Coarse",
            world_model::VolumeRepresentationMode::Coarse},
        std::pair{
            "Passive",
            world_model::VolumeRepresentationMode::Passive},
        std::pair{
            "Baked",
            world_model::VolumeRepresentationMode::Baked}
    };

    for (const auto& [label, mode] : modes)
    {
        const std::string widget =
            std::string(label) +
            "##volume-representation-force-" +
            std::to_string(
                static_cast<i64>(mode));

        if (context.Selectable(
                widget,
                volume->representationMode == mode))
        {
            world.Commands().SetProperty(
                *volumeId,
                world_model::
                    kVolumeRepresentationMode,
                static_cast<i64>(mode));
        }
    }

    context.Text("Follow Target");

    constexpr std::array followModes{
        std::pair{
            "Authored Domain",
            volume_representation::
                FollowTarget::AuthoredDomain},
        std::pair{
            "Camera",
            volume_representation::
                FollowTarget::Camera},
        std::pair{
            "Object / Important Actor",
            volume_representation::
                FollowTarget::Object}
    };

    for (const auto& [label, mode] : followModes)
    {
        const std::string widget =
            std::string(label) +
            "##volume-representation-follow-" +
            std::to_string(
                static_cast<u32>(mode));

        if (context.Selectable(
                widget,
                settings.followTarget == mode))
        {
            settings.followTarget = mode;
        }
    }

    if (settings.followTarget ==
        volume_representation::FollowTarget::Object)
    {
        std::string objectText =
            settings.followObject.has_value()
                ? settings.followObject->ToString()
                : std::string{};

        if (context.InputText(
                "Follow Object ID##volume-representation-follow-object",
                objectText))
        {
            if (objectText.empty())
            {
                settings.followObject.reset();
                settings.
                    followObjectPositionInFrameMeters.
                    reset();
            }
            else if (const auto parsed =
                         scene::ObjectId::Parse(
                             objectText);
                     parsed.has_value())
            {
                settings.followObject =
                    *parsed;
                status_ =
                    "M36 follow object selected. Runtime actor adapter must publish its frame-space position.";
            }
            else
            {
                status_ =
                    "Follow Object ID is not a valid Orbit object ID.";
            }
        }

        bool previewResolvedPosition =
            settings.
                followObjectPositionInFrameMeters.
                has_value();

        if (context.Checkbox(
                "Preview Resolved Actor Position##volume-representation-preview-actor",
                previewResolvedPosition))
        {
            if (previewResolvedPosition)
            {
                settings.
                    followObjectPositionInFrameMeters =
                        diagnostics.
                                followTargetResolved
                            ? diagnostics.
                                  runtimeCenterInFrameMeters
                            : volume->centerMeters;
            }
            else
            {
                settings.
                    followObjectPositionInFrameMeters.
                    reset();
            }
        }

        if (settings.
                followObjectPositionInFrameMeters.
                has_value())
        {
            auto previewPosition =
                *settings.
                    followObjectPositionInFrameMeters;

            if (context.InputDouble3(
                    "Resolved Actor Position##volume-representation-actor-position",
                    previewPosition))
            {
                settings.
                    followObjectPositionInFrameMeters =
                        previewPosition;
            }

            context.MutedText(
                "Preview seam only. Game/runtime actor adapters publish this position automatically; it is not written into the authored Volume center.");
        }
        else
        {
            context.MutedText(
                "Object target unresolved: authored domain center remains active until a runtime actor adapter publishes a frame-space position.");
        }
    }

    f64 liveDistance =
        settings.liveDistanceMeters;
    f64 passiveDistance =
        settings.passiveDistanceMeters;
    f64 livePixels =
        settings.liveProjectedPixels;
    f64 passivePixels =
        settings.passiveProjectedPixels;
    f64 hysteresisPercent =
        static_cast<f64>(
            settings.hysteresisFraction) *
        100.0;
    i64 coarseResolution =
        settings.coarseResolution;
    i64 passiveResolution =
        settings.passiveResolution;
    i64 coarseSteps =
        settings.coarseRaymarchSteps;
    i64 passiveSteps =
        settings.passiveRaymarchSteps;

    bool policyChanged =
        context.InputDouble(
            "Live Radius m##volume-lod-live-distance",
            liveDistance);
    policyChanged |=
        context.InputDouble(
            "Passive Radius m##volume-lod-passive-distance",
            passiveDistance);
    policyChanged |=
        context.InputDouble(
            "Live Projected px##volume-lod-live-pixels",
            livePixels);
    policyChanged |=
        context.InputDouble(
            "Passive Projected px##volume-lod-passive-pixels",
            passivePixels);
    policyChanged |=
        context.InputDouble(
            "Hysteresis %##volume-lod-hysteresis",
            hysteresisPercent);
    policyChanged |=
        context.InputInteger(
            "Coarse Resolution##volume-lod-coarse-resolution",
            coarseResolution);
    policyChanged |=
        context.InputInteger(
            "Passive Resolution##volume-lod-passive-resolution",
            passiveResolution);
    policyChanged |=
        context.InputInteger(
            "Coarse Raymarch Steps##volume-lod-coarse-steps",
            coarseSteps);
    policyChanged |=
        context.InputInteger(
            "Passive Raymarch Steps##volume-lod-passive-steps",
            passiveSteps);

    if (policyChanged)
    {
        settings.liveDistanceMeters =
            std::max(
                liveDistance,
                0.01);
        settings.passiveDistanceMeters =
            std::max(
                passiveDistance,
                settings.liveDistanceMeters +
                    0.01);
        settings.liveProjectedPixels =
            static_cast<f32>(
                std::max(
                    livePixels,
                    1.0));
        settings.passiveProjectedPixels =
            static_cast<f32>(
                std::clamp(
                    passivePixels,
                    0.1,
                    static_cast<f64>(
                        settings.
                            liveProjectedPixels)));
        settings.hysteresisFraction =
            static_cast<f32>(
                std::clamp(
                    hysteresisPercent /
                        100.0,
                    0.0,
                    0.45));
        settings.coarseResolution =
            static_cast<u32>(
                std::clamp<i64>(
                    coarseResolution,
                    8,
                    128));
        settings.passiveResolution =
            static_cast<u32>(
                std::clamp<i64>(
                    passiveResolution,
                    8,
                    64));
        settings.coarseRaymarchSteps =
            static_cast<u32>(
                std::clamp<i64>(
                    coarseSteps,
                    8,
                    96));
        settings.passiveRaymarchSteps =
            static_cast<u32>(
                std::clamp<i64>(
                    passiveSteps,
                    4,
                    32));
    }

    bool showRegions =
        settings.showRepresentationRegions;

    if (context.Checkbox(
            "Visualize Representation Regions##volume-lod-regions",
            showRegions))
    {
        settings.showRepresentationRegions =
            showRegions;
    }

    context.Text(
        std::format(
            "Resolved {}{} | previous {}{}",
            volume_representation::
                ResolvedRepresentationName(
                    diagnostics.representation),
            diagnostics.representationForced
                ? " (forced)"
                : "",
            volume_representation::
                ResolvedRepresentationName(
                    diagnostics.previousRepresentation),
            diagnostics.representationTransition
                ? " | transition active"
                : ""));

    context.Text(
        std::format(
            "Blend Live {:.3f} | Coarse {:.3f} | Passive {:.3f} | Baked {:.3f}",
            diagnostics.liveWeight,
            diagnostics.coarseWeight,
            diagnostics.passiveWeight,
            diagnostics.bakedWeight));

    context.Text(
        std::format(
            "Bounds distance {:.1f} m | projected {:.1f} px | {} field",
            diagnostics.distanceToBoundsMeters,
            diagnostics.projectedDiameterPixels,
            diagnostics.denseFieldRequired
                ? "full live"
                : "bounded aggregate"));

    context.MutedText(
        std::format(
            "Runtime center [{:.2f}, {:.2f}, {:.2f}] | stable 0x{:016X}",
            diagnostics.runtimeCenterInFrameMeters.x,
            diagnostics.runtimeCenterInFrameMeters.y,
            diagnostics.runtimeCenterInFrameMeters.z,
            diagnostics.stableAddressFingerprint));

    if (!diagnostics.followTargetResolved)
    {
        context.MutedText(
            "Follow target is unresolved; runtime safely retains the authored center.");
    }

    if (diagnostics.bakedFallback)
    {
        context.MutedText(
            "Baked was forced but no validated M37 cache is attached; Passive is used explicitly as the bounded fallback.");
    }

    // M37 native cache pipeline. A bake is immediately attached for preview;
    // export is optional. Import validates schema, dimensions and checksum
    // before the renderer can see the cache.
    context.Separator();
    context.Heading("Volume Cache / Bake");
    context.MutedText(
        "Bake authored static density/emission into a versioned .orbitvol asset, or import an existing validated cache. Baked representation resamples it into the current runtime residency tier.");

    i64 bakeResolution =
        static_cast<i64>(cacheBakeResolution_);
    if (context.InputInteger(
            "Bake Resolution##volume-cache-resolution",
            bakeResolution))
    {
        cacheBakeResolution_ =
            static_cast<u32>(
                std::clamp<i64>(
                    bakeResolution,
                    4,
                    512));
    }

    context.InputText(
        "Cache Path##volume-cache-path",
        cachePath_);

    const auto inputs =
        world_model::ResolveVolumeInputs(
            world.Objects(),
            *volumeId);

    const u64 cacheFields =
        volume->fieldMask &
        (static_cast<u64>(
             world_model::VolumeField::Density) |
         static_cast<u64>(
             world_model::VolumeField::Emission));

    const volume_representation::
        VolumeCacheBakeSettings bakeSettings{
            .resolution = cacheBakeResolution_,
            .fieldMask =
                cacheFields != 0U
                    ? cacheFields
                    : static_cast<u64>(
                          world_model::
                              VolumeField::Density)
        };

    if (context.Button(
            "Bake & Attach##volume-cache-bake"))
    {
        auto cache =
            volume_representation::BakeVolumeCache(
                *volume,
                inputs,
                bakeSettings);

        const auto bytes =
            cache.ByteSize();
        const auto fingerprint =
            cache.payloadFingerprint;

        volume_representation::
            VolumeCaches().Attach(
                *volumeId,
                std::move(cache));

        status_ =
            std::format(
                "M37 cache baked: {}^3, {:.2f} MiB, payload 0x{:016X}.",
                cacheBakeResolution_,
                static_cast<double>(bytes) /
                    (1024.0 * 1024.0),
                fingerprint);

        if (!cachePath_.empty())
        {
            const auto* attached =
                volume_representation::
                    VolumeCaches().Find(
                        *volumeId);
            std::string error;
            if (attached != nullptr &&
                !volume_representation::
                    SaveVolumeCache(
                        cachePath_,
                        *attached,
                        &error))
            {
                status_ +=
                    " Export failed: " + error;
            }
            else
            {
                status_ +=
                    " Exported to " + cachePath_ + ".";
            }
        }
    }

    context.SameLine();
    if (context.Button(
            "Import##volume-cache-import"))
    {
        if (cachePath_.empty())
        {
            status_ =
                "Set a .orbitvol path before importing.";
        }
        else
        {
            auto loaded =
                volume_representation::
                    LoadVolumeCache(
                        cachePath_);
            if (loaded)
            {
                volume_representation::
                    VolumeCaches().Attach(
                        *volumeId,
                        std::move(*loaded.cache));
                status_ =
                    "M37 cache imported, checksum validated and attached.";
            }
            else
            {
                status_ =
                    std::format(
                        "Import failed [{}]: {}",
                        volume_representation::
                            VolumeCacheLoadStatusName(
                                loaded.status),
                        loaded.message);
            }
        }
    }

    context.SameLine();
    if (context.Button(
            "Export##volume-cache-export"))
    {
        const auto* cache =
            volume_representation::
                VolumeCaches().Find(
                    *volumeId);
        if (cache == nullptr)
        {
            status_ =
                "No cache is attached to this Volume.";
        }
        else if (cachePath_.empty())
        {
            status_ =
                "Set a .orbitvol path before exporting.";
        }
        else
        {
            std::string error;
            if (volume_representation::
                    SaveVolumeCache(
                        cachePath_,
                        *cache,
                        &error))
            {
                status_ =
                    "M37 cache exported to " +
                    cachePath_ + ".";
            }
            else
            {
                status_ =
                    "Cache export failed: " +
                    error;
            }
        }
    }

    context.SameLine();
    if (context.Button(
            "Detach##volume-cache-detach"))
    {
        volume_representation::
            VolumeCaches().Detach(
                *volumeId);
        status_ =
            "M37 cache detached. Forced Baked will explicitly fall back to Passive.";
    }

    if (const auto* cache =
            volume_representation::
                VolumeCaches().Find(
                    *volumeId);
        cache != nullptr)
    {
        std::string freshness;
        const volume_representation::VolumeCacheBakeSettings
            attachedSettings{
                .resolution =
                    cache->descriptor.resolutionX,
                .fieldMask =
                    cache->descriptor.fieldMask
            };
        const bool current =
            volume_representation::
                IsVolumeCacheCurrent(
                    *cache,
                    *volume,
                    inputs,
                    attachedSettings,
                    &freshness);

        context.Text(
            std::format(
                "Attached {}x{}x{} | {:.2f} MiB | payload 0x{:016X}",
                cache->descriptor.resolutionX,
                cache->descriptor.resolutionY,
                cache->descriptor.resolutionZ,
                static_cast<double>(
                    cache->ByteSize()) /
                    (1024.0 * 1024.0),
                cache->payloadFingerprint));
        context.MutedText(
            current
                ? "Cache matches current authored inputs and bake settings."
                : freshness);
        if (!cache->sourcePath.empty())
        {
            context.MutedText(
                "Imported from: " +
                cache->sourcePath);
        }
    }
    else
    {
        context.MutedText(
            "No cache attached. Baked remains unavailable and cannot silently masquerade as live data.");
    }

    context.Separator();
    context.Heading("Particle / Surface Output");
    context.MutedText(
        "M38 samples the authoritative volume field on simulation time and publishes bounded particle and physical-surface requests. The current CPU-readable producer is the validated M37 cache; live GPU fields will use the same output contract through compact GPU production.");

    bool particlesEnabled = volume->outputParticlesEnabled;
    if (context.Checkbox("Particles##volume-output-particles", particlesEnabled))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputParticlesEnabled, particlesEnabled);

    bool depositsEnabled = volume->outputSurfaceDepositsEnabled;
    if (context.Checkbox("Surface Deposits##volume-output-deposits", depositsEnabled))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputSurfaceDepositsEnabled, depositsEnabled);

    f64 threshold = volume->outputFieldThreshold;
    if (context.InputDouble("Field Threshold##volume-output-threshold", threshold))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputFieldThreshold, std::clamp(threshold, 0.0, 64.0));

    f64 particleRate = volume->outputParticleRatePerSecond;
    i64 particleBudget = static_cast<i64>(volume->outputParticleBudgetPerStep);
    if (context.InputDouble("Particle Rate / s##volume-output-particle-rate", particleRate))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputParticleRate, std::clamp(particleRate, 0.0, 1000000.0));
    if (context.InputInteger("Particle Budget / Step##volume-output-particle-budget", particleBudget))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputParticleBudget, std::clamp<i64>(particleBudget, 1, 1000000));

    f64 lifetime = volume->particleLifetimeSeconds;
    f64 drag = volume->particleLinearDragPerSecond;
    f64 particleRadius = volume->particleRadiusMeters;
    f64 particleEmissionScale = volume->particleEmissionScale;
    if (context.InputDouble("Particle Lifetime s##volume-output-particle-life", lifetime))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleLifetime, std::clamp(lifetime, 0.001, 3600.0));
    if (context.InputDouble("Particle Linear Drag / s##volume-output-particle-drag", drag))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleLinearDrag, std::clamp(drag, 0.0, 100.0));
    if (context.InputDouble("Particle Radius m##volume-output-particle-radius", particleRadius))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleRadiusMeters, std::clamp(particleRadius, 0.001, 100.0));
    if (context.InputDouble("Particle Emission Scale##volume-output-particle-emission", particleEmissionScale))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleEmissionScale, std::clamp(particleEmissionScale, 0.0, 1024.0));

    context.Text("Particle Gravity");
    for (const auto& [label, mode] : std::array{
             std::pair{"None", world_model::VolumeParticleGravityMode::None},
             std::pair{"Owning Body", world_model::VolumeParticleGravityMode::OwningBody}})
        if (context.Selectable(std::string(label) + "##volume-output-gravity-" + std::to_string(static_cast<i64>(mode)), volume->particleGravityMode == mode))
            world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleGravityMode, static_cast<i64>(mode));

    f64 gravityScale = volume->particleGravityScale;
    if (context.InputDouble("Gravity Scale##volume-output-gravity-scale", gravityScale))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleGravityScale, std::clamp(gravityScale, 0.0, 16.0));

    context.Text("Particle Collision");
    for (const auto& [label, mode] : std::array{
             std::pair{"None", world_model::VolumeParticleCollisionMode::None},
             std::pair{"Kill", world_model::VolumeParticleCollisionMode::Kill},
             std::pair{"Slide", world_model::VolumeParticleCollisionMode::Slide},
             std::pair{"Bounce", world_model::VolumeParticleCollisionMode::Bounce}})
        if (context.Selectable(std::string(label) + "##volume-output-collision-" + std::to_string(static_cast<i64>(mode)), volume->particleCollisionMode == mode))
            world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleCollisionMode, static_cast<i64>(mode));

    f64 restitution = volume->particleRestitution;
    if (context.InputDouble("Restitution##volume-output-restitution", restitution))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeParticleRestitution, std::clamp(restitution, 0.0, 1.0));

    f64 depositRate = volume->outputSurfaceDepositRatePerSecond;
    i64 depositBudget = static_cast<i64>(volume->outputSurfaceDepositBudgetPerStep);
    f64 depositRadius = volume->outputSurfaceDepositRadiusMeters;
    if (context.InputDouble("Deposit Rate / s##volume-output-deposit-rate", depositRate))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputSurfaceRate, std::clamp(depositRate, 0.0, 1000000.0));
    if (context.InputInteger("Deposit Budget / Step##volume-output-deposit-budget", depositBudget))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputSurfaceBudget, std::clamp<i64>(depositBudget, 1, 1000000));
    if (context.InputDouble("Deposit Radius m##volume-output-deposit-radius", depositRadius))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputSurfaceRadius, std::clamp(depositRadius, 0.001, 10000.0));

    context.Text("Surface Effect");
    for (const auto& [label, effect] : std::array{
             std::pair{"Wetness", world_model::VolumeSurfaceOutputEffect::Wetness},
             std::pair{"Soot", world_model::VolumeSurfaceOutputEffect::Soot},
             std::pair{"Ash", world_model::VolumeSurfaceOutputEffect::Ash},
             std::pair{"Sediment", world_model::VolumeSurfaceOutputEffect::Sediment},
             std::pair{"Heat", world_model::VolumeSurfaceOutputEffect::Heat}})
        if (context.Selectable(std::string(label) + "##volume-output-effect-" + std::to_string(static_cast<i64>(effect)), volume->outputSurfaceEffect == effect))
            world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputSurfaceEffect, static_cast<i64>(effect));

    f64 halfLife = volume->outputSurfaceEffectHalfLifeSeconds;
    if (context.InputDouble("Surface Effect Half Life s##volume-output-effect-half-life", halfLife))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputSurfaceHalfLife, std::clamp(halfLife, 0.0, 86400.0));

    i64 candidateMultiplier = static_cast<i64>(volume->outputCandidateMultiplier);
    if (context.InputInteger("Candidate Multiplier##volume-output-candidates", candidateMultiplier))
        world.Commands().SetProperty(*volumeId, world_model::kVolumeOutputCandidateMultiplier, std::clamp<i64>(candidateMultiplier, 1, 64));

    context.MutedText("Gravity/collision policies are authored and carried per particle. GPU body-gravity and physical-surface response are the next M38 simulation hook.");

    if ((volume->outputParticlesEnabled ||
         volume->outputSurfaceDepositsEnabled) &&
        volume_representation::
            VolumeCaches().Find(
                *volumeId) == nullptr)
    {
        context.MutedText(
            "Output is enabled but this Volume has no CPU-readable field authority yet. No synthetic events are emitted; attach a current M37 cache or use the upcoming live GPU producer.");
    }

    if (const auto* output =
            volume_representation::
                VolumeOutputs().Latest(
                    *volumeId);
        output != nullptr)
    {
        const auto& d =
            output->diagnostics;

        context.Text(
            std::format(
                "Last step {:.4f} s | particles {}/{} | deposits {}/{}",
                d.deltaSeconds,
                d.emittedParticles,
                d.requestedParticles,
                d.emittedSurfaceDeposits,
                d.requestedSurfaceDeposits));
        context.MutedText(
            std::format(
                "Candidates {} | threshold rejects {} | budget drops P {} / S {}",
                d.candidatesTested,
                d.thresholdRejected,
                d.particleBudgetDropped,
                d.surfaceBudgetDropped));
    }

    const auto& runtimeDiagnostics =
        volume_representation::
            VolumeOutputRuntimeService().
                Diagnostics();

    context.MutedText(
        std::format(
            "World runtime: {} volumes | {} eligible | {} advanced | {} without readable authority | dispatched P {} / S {}",
            runtimeDiagnostics.discoveredVolumes,
            runtimeDiagnostics.eligibleVolumes,
            runtimeDiagnostics.advancedVolumes,
            runtimeDiagnostics.volumesWithoutReadableAuthority,
            runtimeDiagnostics.dispatchedParticleRequests,
            runtimeDiagnostics.dispatchedSurfaceRequests));

    if (runtimeDiagnostics.timeReversed)
    {
        context.MutedText(
            "Simulation time moved backward this step; M38 sequence/carry state was reset deterministically.");
    }

    context.MutedText(
        std::format(
            "Pending consumer queues: particles {} | surface requests {}",
            volume_representation::
                VolumeParticleRequests().
                    Pending().size(),
            volume_representation::
                VolumeSurfaceRequests().
                    Pending().size()));

    if (!status_.empty())
    {
        context.MutedText(status_);
    }

    if (settings.showRepresentationRegions)
    {
        const auto available =
            context.ContentAvailable();
        const f32 width =
            std::max(
                std::min(
                    available.width,
                    420.0F),
                220.0F);
        constexpr f32 height =
            190.0F;

        static_cast<void>(
            context.Canvas(
                "##volume-lod-region-map",
                {
                    .width = width,
                    .height = height
                }));

        const math::Float2 center{
            width * 0.5F,
            height * 0.52F
        };

        const f64 outerMeters =
            std::max(
                settings.passiveDistanceMeters,
                settings.liveDistanceMeters +
                    0.01);
        const f32 outerRadius =
            std::max(
                std::min(
                    width,
                    height) * 0.42F,
                20.0F);
        const f32 liveRadius =
            static_cast<f32>(
                settings.liveDistanceMeters /
                outerMeters) *
            outerRadius;

        context.CanvasCircle(
            center,
            outerRadius,
            {0.42F,0.44F,0.48F,0.85F},
            false,
            2.0F);
        context.CanvasCircle(
            center,
            liveRadius,
            {0.20F,0.82F,0.44F,0.95F},
            false,
            2.0F);

        const f32 observerRadius =
            static_cast<f32>(
                std::clamp(
                    diagnostics.
                        distanceToBoundsMeters /
                    outerMeters,
                    0.0,
                    1.0)) *
            outerRadius;

        context.CanvasCircle(
            {
                center.x + observerRadius,
                center.y
            },
            4.0F,
            {1.0F,0.82F,0.18F,1.0F},
            true);

        context.CanvasCircle(
            center,
            4.0F,
            {0.25F,0.72F,1.0F,1.0F},
            true);

        context.CanvasText(
            {8.0F,8.0F},
            {0.86F,0.88F,0.92F,1.0F},
            "Live / Coarse / Passive policy map");
        context.CanvasText(
            {8.0F,height - 34.0F},
            {0.70F,0.73F,0.78F,1.0F},
            std::format(
                "Live <= {:.0f} m | Passive >= {:.0f} m",
                settings.liveDistanceMeters,
                settings.passiveDistanceMeters));
        context.CanvasText(
            {8.0F,height - 18.0F},
            {0.70F,0.73F,0.78F,1.0F},
            std::format(
                "Current: {} | {:.1f} px",
                volume_representation::
                    ResolvedRepresentationName(
                        diagnostics.representation),
                diagnostics.projectedDiameterPixels));
    }
}
} // namespace orbit::studio_ui
