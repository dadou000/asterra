#include <orbit/studio_ui/DisplayDiagnosticsUi.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace orbit::studio_ui
{
DisplayDiagnosticsUi::DisplayDiagnosticsUi(
    StudioViewportRenderer& renderer) noexcept
    : renderer_(&renderer)
{
}

void DisplayDiagnosticsUi::Register(
    editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Display Diagnostics",
        .defaultOpen = false,
        .defaultDock =
            editor_ui::DockRegion::Right,
        .dockOrder = 55,
        .minSize = {
            .width = 320.0F,
            .height = 260.0F
        },
        .draw =
            [this](
                editor_ui::PanelContext& context)
            {
                DrawViewport(
                    context,
                    "studio.primary",
                    "Primary View");

                context.Separator();

                DrawViewport(
                    context,
                    "studio.map",
                    "Body Map");
            }
    });
}

void DisplayDiagnosticsUi::DrawViewport(
    editor_ui::PanelContext& context,
    const std::string_view viewportId,
    const std::string_view label)
{
    if (renderer_ == nullptr)
    {
        context.MutedText(
            "Display renderer is unavailable.");
        return;
    }

    context.Heading(label);

    auto diagnostics =
        renderer_->
            LuminanceHistogramDiagnostics(
                viewportId);

    if (!diagnostics.has_value())
    {
        context.MutedText(
            "Waiting for HDR metering data.");
        return;
    }

    const auto& stats =
        diagnostics->statistics;

    if (!stats.valid)
    {
        context.MutedText(
            "Histogram reduction has not completed yet.");
    }
    else
    {
        context.Text(
            std::format(
                "P50 {:8.4f}  | P90 {:8.4f}",
                stats.medianLuminance,
                stats.p90Luminance));

        context.Text(
            std::format(
                "P95 {:8.4f}  | P99 {:8.4f}",
                stats.p95Luminance,
                stats.p99Luminance));

        context.Text(
            std::format(
                "Peak {:8.4f}  | Geometric mean {:8.4f}",
                stats.peakLuminance,
                stats.geometricMeanLuminance));

        context.MutedText(
            std::format(
                "{} samples | {} weighted units",
                stats.sampleCount,
                stats.weightedSampleCount));

        const auto& eye =
            diagnostics->eyeState;

        context.Separator();
        context.Text("Human Eye State");

        if (!eye.initialized)
        {
            context.MutedText(
                "Waiting for the first valid adaptation sample.");
        }
        else
        {
            context.Text(
                std::format(
                    "Photopic {:7.3f} stops  | target {:7.3f}",
                    eye.photopicLog2,
                    eye.photopicTargetLog2));

            context.Text(
                std::format(
                    "Dark adaptation {:6.1f}%  | target {:6.1f}%",
                    eye.darkAdaptation * 100.0F,
                    eye.darkTarget * 100.0F));

            context.Text(
                std::format(
                    "Overload {:6.1f}%  | target {:6.1f}%",
                    eye.overload * 100.0F,
                    eye.overloadTarget * 100.0F));

            context.Text(
                std::format(
                    "Exposure x{:8.4f}  | target x{:8.4f}",
                    eye.exposureScale,
                    eye.targetExposureScale));

            context.Text(
                std::format(
                    "Ceiling excess {:6.2f} stops  | P99 {:6.2f} | Peak {:6.2f}",
                    eye.photopicCeilingExcessStops,
                    eye.p99ExcessStops,
                    eye.peakExcessStops));
        }

        const auto available =
            context.ContentAvailable();

        const f32 canvasWidth =
            std::max(
                available.width,
                220.0F);
        constexpr f32 kCanvasHeight =
            150.0F;

        const std::string canvasId =
            "Luminance Histogram##" +
            std::string(viewportId);

        static_cast<void>(
            context.Canvas(
                canvasId,
                {
                    .width = canvasWidth,
                    .height = kCanvasHeight
                }));

        u32 maximumBin = 1U;

        for (const u32 count :
             diagnostics->bins)
        {
            maximumBin =
                std::max(
                    maximumBin,
                    count);
        }

        const f32 binWidth =
            canvasWidth /
            static_cast<f32>(
                post_process::
                    kLuminanceHistogramBins);

        for (u32 bin = 0U;
             bin <
                 post_process::
                     kLuminanceHistogramBins;
             ++bin)
        {
            const f32 normalized =
                static_cast<f32>(
                    diagnostics->bins[bin]) /
                static_cast<f32>(
                    maximumBin);

            const f32 x =
                (static_cast<f32>(bin) +
                 0.5F) *
                binWidth;

            context.CanvasLine(
                {x, kCanvasHeight - 2.0F},
                {
                    x,
                    kCanvasHeight -
                        2.0F -
                        normalized *
                            (kCanvasHeight -
                             18.0F)
                },
                {0.72F, 0.78F, 0.86F, 1.0F},
                std::max(
                    binWidth * 0.75F,
                    1.0F));
        }

        const auto percentileX =
            [&](const f32 log2Luminance)
            {
                const f32 range =
                    std::max(
                        diagnostics->
                                config.
                                maximumLog2 -
                            diagnostics->
                                config.
                                minimumLog2,
                        1.0e-4F);

                return
                    std::clamp(
                        (log2Luminance -
                         diagnostics->
                             config.
                             minimumLog2) /
                            range,
                        0.0F,
                        1.0F) *
                    canvasWidth;
            };

        const auto marker =
            [&](const f32 value,
                const std::string_view name,
                const math::Float4 color)
            {
                const f32 x =
                    percentileX(value);

                context.CanvasLine(
                    {x, 0.0F},
                    {x, kCanvasHeight},
                    color,
                    1.0F);
                context.CanvasText(
                    {x + 2.0F, 2.0F},
                    color,
                    name);
            };

        marker(
            stats.medianLog2,
            "P50",
            {0.30F, 0.88F, 0.55F, 1.0F});
        marker(
            stats.p95Log2,
            "P95",
            {0.95F, 0.76F, 0.28F, 1.0F});
        marker(
            stats.p99Log2,
            "P99",
            {0.96F, 0.48F, 0.22F, 1.0F});
        marker(
            stats.peakLog2,
            "Peak",
            {0.95F, 0.28F, 0.25F, 1.0F});
        marker(
            diagnostics->eyeConfig.photopicCeilingLog2,
            "Ceiling",
            {0.72F, 0.38F, 0.96F, 1.0F});
    }

    context.Separator();
    context.Text("Metering");

    auto config =
        diagnostics->config;

    f64 minimumLog2 =
        config.minimumLog2;
    f64 maximumLog2 =
        config.maximumLog2;
    f64 centerWeight =
        config.centerWeightStrength;
    f64 centerRadius =
        config.centerWeightRadius;

    bool configChanged = false;

    configChanged |=
        context.InputDouble(
            ("Minimum log2##meter-min-" +
             std::string(viewportId)),
            minimumLog2);
    configChanged |=
        context.InputDouble(
            ("Maximum log2##meter-max-" +
             std::string(viewportId)),
            maximumLog2);
    configChanged |=
        context.InputDouble(
            ("Center Weight##meter-center-" +
             std::string(viewportId)),
            centerWeight);
    configChanged |=
        context.InputDouble(
            ("Center Radius##meter-radius-" +
             std::string(viewportId)),
            centerRadius);

    if (configChanged)
    {
        config.minimumLog2 =
            static_cast<f32>(
                minimumLog2);
        config.maximumLog2 =
            static_cast<f32>(
                maximumLog2);
        config.centerWeightStrength =
            static_cast<f32>(
                centerWeight);
        config.centerWeightRadius =
            static_cast<f32>(
                centerRadius);

        renderer_->
            SetLuminanceHistogramConfig(
                viewportId,
                config);
    }

    if (context.Button(
            "Reset Metering##reset-metering-" +
            std::string(viewportId)))
    {
        renderer_->
            SetLuminanceHistogramConfig(
                viewportId,
                {});
    }

    context.Separator();
    context.Text("Eye Adaptation");

    auto eyeConfig =
        diagnostics->eyeConfig;

    f64 p50Weight =
        eyeConfig.photopicP50Weight;
    f64 p95Weight =
        eyeConfig.photopicP95Weight;
    f64 brightenSeconds =
        eyeConfig.photopicBrightenSeconds;
    f64 darkenSeconds =
        eyeConfig.photopicDarkenSeconds;
    f64 photopicCeiling =
        eyeConfig.photopicCeilingLog2;
    f64 ceilingRecoverySeconds =
        eyeConfig.photopicCeilingRecoverySeconds;
    f64 exposureMiddleGray =
        eyeConfig.exposureMiddleGray;
    f64 minimumExposure =
        eyeConfig.minimumExposureScale;
    f64 maximumExposure =
        eyeConfig.maximumExposureScale;
    f64 darkThreshold =
        eyeConfig.darkThresholdLog2;
    f64 darkFull =
        eyeConfig.darkFullLog2;
    f64 darkAdaptSeconds =
        eyeConfig.darkAdaptSeconds;
    f64 darkResetSeconds =
        eyeConfig.darkResetSeconds;
    f64 overloadP99 =
        eyeConfig.overloadP99StartStops;
    f64 overloadPeak =
        eyeConfig.overloadPeakStartStops;
    f64 overloadRange =
        eyeConfig.overloadSoftRangeStops;
    f64 overloadAttack =
        eyeConfig.overloadAttackSeconds;
    f64 overloadRecovery =
        eyeConfig.overloadRecoverySeconds;

    bool eyeConfigChanged = false;

    eyeConfigChanged |=
        context.InputDouble(
            ("Photopic P50 Weight##eye-p50-" +
             std::string(viewportId)),
            p50Weight);
    eyeConfigChanged |=
        context.InputDouble(
            ("Photopic P95 Weight##eye-p95-" +
             std::string(viewportId)),
            p95Weight);
    eyeConfigChanged |=
        context.InputDouble(
            ("Bright Adapt Seconds##eye-bright-" +
             std::string(viewportId)),
            brightenSeconds);
    eyeConfigChanged |=
        context.InputDouble(
            ("Darken Seconds##eye-darken-" +
             std::string(viewportId)),
            darkenSeconds);
    eyeConfigChanged |=
        context.InputDouble(
            ("Photopic Ceiling log2##eye-ceiling-" +
             std::string(viewportId)),
            photopicCeiling);
    eyeConfigChanged |=
        context.InputDouble(
            ("Ceiling Recovery Seconds##eye-ceiling-recovery-" +
             std::string(viewportId)),
            ceilingRecoverySeconds);
    eyeConfigChanged |=
        context.InputDouble(
            ("Exposure Middle Gray##eye-middle-gray-" +
             std::string(viewportId)),
            exposureMiddleGray);
    eyeConfigChanged |=
        context.InputDouble(
            ("Minimum Exposure Scale##eye-min-exposure-" +
             std::string(viewportId)),
            minimumExposure);
    eyeConfigChanged |=
        context.InputDouble(
            ("Maximum Exposure Scale##eye-max-exposure-" +
             std::string(viewportId)),
            maximumExposure);
    eyeConfigChanged |=
        context.InputDouble(
            ("Dark Threshold log2##eye-dark-threshold-" +
             std::string(viewportId)),
            darkThreshold);
    eyeConfigChanged |=
        context.InputDouble(
            ("Full Dark log2##eye-dark-full-" +
             std::string(viewportId)),
            darkFull);
    eyeConfigChanged |=
        context.InputDouble(
            ("Dark Adapt Seconds##eye-dark-adapt-" +
             std::string(viewportId)),
            darkAdaptSeconds);
    eyeConfigChanged |=
        context.InputDouble(
            ("Dark Reset Seconds##eye-dark-reset-" +
             std::string(viewportId)),
            darkResetSeconds);
    eyeConfigChanged |=
        context.InputDouble(
            ("P99 Overload Start##eye-overload-p99-" +
             std::string(viewportId)),
            overloadP99);
    eyeConfigChanged |=
        context.InputDouble(
            ("Peak Overload Start##eye-overload-peak-" +
             std::string(viewportId)),
            overloadPeak);
    eyeConfigChanged |=
        context.InputDouble(
            ("Overload Soft Range##eye-overload-range-" +
             std::string(viewportId)),
            overloadRange);
    eyeConfigChanged |=
        context.InputDouble(
            ("Overload Attack Seconds##eye-overload-attack-" +
             std::string(viewportId)),
            overloadAttack);
    eyeConfigChanged |=
        context.InputDouble(
            ("Overload Recovery Seconds##eye-overload-recovery-" +
             std::string(viewportId)),
            overloadRecovery);

    if (eyeConfigChanged)
    {
        eyeConfig.photopicP50Weight =
            static_cast<f32>(p50Weight);
        eyeConfig.photopicP95Weight =
            static_cast<f32>(p95Weight);
        eyeConfig.photopicBrightenSeconds =
            static_cast<f32>(brightenSeconds);
        eyeConfig.photopicDarkenSeconds =
            static_cast<f32>(darkenSeconds);
        eyeConfig.photopicCeilingLog2 =
            static_cast<f32>(photopicCeiling);
        eyeConfig.photopicCeilingRecoverySeconds =
            static_cast<f32>(ceilingRecoverySeconds);
        eyeConfig.exposureMiddleGray =
            static_cast<f32>(exposureMiddleGray);
        eyeConfig.minimumExposureScale =
            static_cast<f32>(minimumExposure);
        eyeConfig.maximumExposureScale =
            static_cast<f32>(maximumExposure);
        eyeConfig.darkThresholdLog2 =
            static_cast<f32>(darkThreshold);
        eyeConfig.darkFullLog2 =
            static_cast<f32>(darkFull);
        eyeConfig.darkAdaptSeconds =
            static_cast<f32>(darkAdaptSeconds);
        eyeConfig.darkResetSeconds =
            static_cast<f32>(darkResetSeconds);
        eyeConfig.overloadP99StartStops =
            static_cast<f32>(overloadP99);
        eyeConfig.overloadPeakStartStops =
            static_cast<f32>(overloadPeak);
        eyeConfig.overloadSoftRangeStops =
            static_cast<f32>(overloadRange);
        eyeConfig.overloadAttackSeconds =
            static_cast<f32>(overloadAttack);
        eyeConfig.overloadRecoverySeconds =
            static_cast<f32>(overloadRecovery);

        renderer_->
            SetHumanEyeAdaptationConfig(
                viewportId,
                eyeConfig);
    }

    if (context.Button(
            "Reset Eye State##reset-eye-" +
            std::string(viewportId)))
    {
        renderer_->
            ResetHumanEyeAdaptation(
                viewportId);
    }

    if (context.Button(
            "Reset Eye Defaults##reset-eye-defaults-" +
            std::string(viewportId)))
    {
        renderer_->
            SetHumanEyeAdaptationConfig(
                viewportId,
                {});
        renderer_->
            ResetHumanEyeAdaptation(
                viewportId);
    }

    bool overlay =
        renderer_->
            LuminanceMeteringOverlay(
                viewportId);

    if (context.Checkbox(
            "Viewport Metering Overlay##meter-overlay-" +
                std::string(viewportId),
            overlay))
    {
        renderer_->
            SetLuminanceMeteringOverlay(
                viewportId,
                overlay);
    }

    if (auto* mask =
            renderer_->
                LuminanceMeteringMask(
                    viewportId);
        mask != nullptr &&
        diagnostics->
            meteringMaskAvailable)
    {
        context.Text(
            "Metering Mask / Log-Luminance");

        const auto available =
            context.ContentAvailable();

        const f32 previewWidth =
            std::max(
                available.width,
                220.0F);

        const f32 aspect =
            mask->Height() > 0U
                ? static_cast<f32>(
                      mask->Width()) /
                  static_cast<f32>(
                      mask->Height())
                : 1.0F;

        const f32 previewHeight =
            std::clamp(
                previewWidth /
                    std::max(aspect, 0.1F),
                90.0F,
                220.0F);

        static_cast<void>(
            context.Image(
                *mask,
                {
                    .width = previewWidth,
                    .height = previewHeight
                }));
    }
}
} // namespace orbit::studio_ui
