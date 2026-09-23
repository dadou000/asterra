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
            "Blend Live {:.3f} | Coarse {:.3f} | Passive {:.3f}",
            diagnostics.liveWeight,
            diagnostics.coarseWeight,
            diagnostics.passiveWeight));

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
            "Baked was forced but M37 has not supplied a valid cache yet; Passive is used explicitly as the bounded fallback.");
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
