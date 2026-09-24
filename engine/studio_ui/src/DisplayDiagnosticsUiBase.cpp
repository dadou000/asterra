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

    if (viewportId == "studio.primary")
    {
        context.Separator();
        context.Text("Color LUT");

        const auto lutDiagnostics =
            renderer_->
                ColorLutDiagnostics();

        context.Text(
            std::format(
                "{} | {}^3 | {} / {}",
                lutDiagnostics.sourcePath,
                lutDiagnostics.size,
                post_process::
                    ColorLutDomainName(
                        lutDiagnostics.domain),
                post_process::
                    ColorLutShaperName(
                        lutDiagnostics.shaper)));

        if (!lutDiagnostics.title.empty())
        {
            context.MutedText(
                std::format(
                    "Title: {}",
                    lutDiagnostics.title));
        }

        context.MutedText(
            lutDiagnostics.explicitMetadata
                ? "Orbit domain/shaper metadata: explicit"
                : "Orbit domain/shaper metadata: inferred default");

        if (!lutDiagnostics.diagnostic.empty())
        {
            context.MutedText(
                lutDiagnostics.diagnostic);
        }

        auto lutSettings =
            renderer_->
                ColorLutSettings();

        bool lutEnabled =
            lutSettings.enabled;
        bool lutSettingsChanged =
            context.Checkbox(
                "Enable LUT##color-lut-enabled",
                lutEnabled);

        f64 lutStrength =
            lutSettings.strength;

        lutSettingsChanged |=
            context.InputDouble(
                "LUT Strength##color-lut-strength",
                lutStrength);

        lutSettings.enabled =
            lutEnabled;
        lutSettings.strength =
            static_cast<f32>(
                lutStrength);

        if (lutSettingsChanged)
        {
            renderer_->
                SetColorLutSettings(
                    lutSettings);
        }

        context.Text("Project LUT Assets");

        for (const auto& path :
             renderer_->
                 ColorLutAssetPaths())
        {
            if (context.Selectable(
                    path,
                    path ==
                        lutDiagnostics.
                            sourcePath))
            {
                static_cast<void>(
                    renderer_->
                        SelectColorLutAsset(
                            path));
            }
        }

        static std::string
            importLutPath;

        static_cast<void>(
            context.InputText(
                "External .cube Path##color-lut-import-path",
                importLutPath));

        if (context.Button(
                "Import & Select LUT##color-lut-import"))
        {
            static_cast<void>(
                renderer_->
                    ImportColorLutFile(
                        importLutPath));
        }

        if (context.Button(
                "Reset Identity LUT##color-lut-reset"))
        {
            renderer_->
                SetColorLut(
                    post_process::
                        BuildIdentityColorLut());
        }
    }

    if (viewportId == "studio.primary")
    {
        context.Separator();
        context.Text("Output Transform");

        auto output =
            renderer_->
                OutputTransformDiagnostics();

        context.Text(
            std::format(
                "Requested {} | resolved {}",
                post_process::
                    OutputModeName(
                        output.settings.mode),
                post_process::
                    OutputModeName(
                        output.resolved.resolvedMode)));

        context.Text(
            std::format(
                "Reference white {:7.1f} nits | output peak {:7.1f} nits",
                output.resolved.referenceWhiteNits,
                output.resolved.resolvedPeakNits));

        if (output.capabilities.hdr10Supported)
        {
            context.MutedText(
                std::format(
                    "HDR10 presentation capability available | reported peak {:7.1f} nits",
                    output.capabilities.reportedPeakNits));
        }
        else
        {
            context.MutedText(
                "HDR10 presentation capability unavailable; Auto/HDR safely resolve to SDR.");
        }

        if (output.resolved.fellBackToSdr)
        {
            context.MutedText(
                "HDR was requested but no compatible HDR10 output surface is available.");
        }

        if (context.Selectable(
                "Auto##output-auto",
                output.settings.mode ==
                    post_process::
                        OutputMode::Auto))
        {
            output.settings.mode =
                post_process::
                    OutputMode::Auto;
            renderer_->
                SetOutputTransformSettings(
                    output.settings);
        }

        if (context.Selectable(
                "SDR##output-sdr",
                output.settings.mode ==
                    post_process::
                        OutputMode::Sdr))
        {
            output.settings.mode =
                post_process::
                    OutputMode::Sdr;
            renderer_->
                SetOutputTransformSettings(
                    output.settings);
        }

        if (context.Selectable(
                "HDR10 / PQ##output-hdr10",
                output.settings.mode ==
                    post_process::
                        OutputMode::Hdr10))
        {
            output.settings.mode =
                post_process::
                    OutputMode::Hdr10;
            renderer_->
                SetOutputTransformSettings(
                    output.settings);
        }

        f64 outputWhite =
            output.settings.referenceWhiteNits;
        f64 outputPeak =
            output.settings.requestedPeakNits;

        bool outputChanged =
            context.InputDouble(
                "Output Reference White nits##output-reference-white",
                outputWhite);

        outputChanged |=
            context.InputDouble(
                "Requested HDR Peak nits##output-peak",
                outputPeak);

        output.settings.referenceWhiteNits =
            static_cast<f32>(
                outputWhite);
        output.settings.requestedPeakNits =
            static_cast<f32>(
                outputPeak);

        if (outputChanged)
        {
            renderer_->
                SetOutputTransformSettings(
                    output.settings);
        }

        context.Text(
            std::format(
                "Test pattern: {}",
                post_process::
                    OutputTestPatternName(
                        output.settings.testPattern)));

        if (context.Button(
                "Pattern Off##output-pattern-off"))
        {
            output.settings.testPattern =
                post_process::
                    OutputTestPattern::None;
            renderer_->
                SetOutputTransformSettings(
                    output.settings);
        }

        if (context.Button(
                "Linear Ramp##output-pattern-ramp"))
        {
            output.settings.testPattern =
                post_process::
                    OutputTestPattern::LinearRamp;
            renderer_->
                SetOutputTransformSettings(
                    output.settings);
        }

        if (context.Button(
                "Reference White##output-pattern-reference"))
        {
            output.settings.testPattern =
                post_process::
                    OutputTestPattern::ReferenceWhite;
            renderer_->
                SetOutputTransformSettings(
                    output.settings);
        }

        if (context.Button(
                "Peak White##output-pattern-peak"))
        {
            output.settings.testPattern =
                post_process::
                    OutputTestPattern::PeakWhite;
            renderer_->
                SetOutputTransformSettings(
                    output.settings);
        }

        if (context.Button(
                "Reset Output Defaults##output-reset"))
        {
            renderer_->
                SetOutputTransformSettings(
                    {});
        }
    }

    context.Separator();
    context.Text("Tone Mapping / Display Headroom");

    auto toneMapping =
        diagnostics->toneMapping;

    const auto toneDiagnostics =
        post_process::
            EvaluateToneMappingDiagnostics(
                toneMapping);

    context.Text(
        std::format(
            "Reference white {:7.1f} nits  | peak {:7.1f} nits",
            toneMapping.referenceWhiteNits,
            toneMapping.peakNits));
    context.Text(
        std::format(
            "Linear headroom x{:6.3f}  | mapped white {:6.3f}",
            toneDiagnostics.headroomRatio,
            toneDiagnostics.mappedReferenceWhite));

    bool toneEnabled =
        toneMapping.enabled;
    bool toneChanged =
        context.Checkbox(
            "Production Tone Map##tone-enabled-" +
                std::string(viewportId),
            toneEnabled);

    f64 referenceWhite =
        toneMapping.referenceWhiteNits;
    f64 peakNits =
        toneMapping.peakNits;
    f64 shoulderStart =
        toneMapping.shoulderStart;
    f64 shoulderStrength =
        toneMapping.shoulderStrength;

    toneChanged |=
        context.InputDouble(
            ("Reference White nits##tone-white-" +
             std::string(viewportId)),
            referenceWhite);
    toneChanged |=
        context.InputDouble(
            ("Display Peak nits##tone-peak-" +
             std::string(viewportId)),
            peakNits);
    toneChanged |=
        context.InputDouble(
            ("Shoulder Start##tone-shoulder-start-" +
             std::string(viewportId)),
            shoulderStart);
    toneChanged |=
        context.InputDouble(
            ("Shoulder Strength##tone-shoulder-strength-" +
             std::string(viewportId)),
            shoulderStrength);

    toneMapping.enabled =
        toneEnabled;
    toneMapping.referenceWhiteNits =
        static_cast<f32>(referenceWhite);
    toneMapping.peakNits =
        static_cast<f32>(peakNits);
    toneMapping.shoulderStart =
        static_cast<f32>(shoulderStart);
    toneMapping.shoulderStrength =
        static_cast<f32>(shoulderStrength);

    if (context.Button(
            "Reset Tone Defaults##tone-reset-" +
            std::string(viewportId)))
    {
        toneMapping = {};
        toneChanged = true;
    }

    if (toneChanged)
    {
        renderer_->
            SetToneMappingConfig(
                viewportId,
                toneMapping);
    }

    context.Separator();
    context.Text("Bloom / Glare / Flare");

    auto highlightConfig =
        diagnostics->highlightConfig;

    bool bloomEnabled =
        highlightConfig.bloomEnabled;
    bool glareEnabled =
        highlightConfig.glareEnabled;
    bool flareEnabled =
        highlightConfig.flareEnabled;

    bool highlightChanged = false;

    highlightChanged |=
        context.Checkbox(
            "Bloom##highlight-bloom-" +
                std::string(viewportId),
            bloomEnabled);
    highlightChanged |=
        context.Checkbox(
            "Glare##highlight-glare-" +
                std::string(viewportId),
            glareEnabled);
    highlightChanged |=
        context.Checkbox(
            "Flare##highlight-flare-" +
                std::string(viewportId),
            flareEnabled);

    f64 bloomThreshold =
        highlightConfig.bloomThreshold;
    f64 bloomKnee =
        highlightConfig.bloomKnee;
    f64 bloomStrength =
        highlightConfig.bloomStrength;
    f64 bloomRadius =
        highlightConfig.bloomRadiusPixels;

    f64 glareThreshold =
        highlightConfig.glareThreshold;
    f64 glareStrength =
        highlightConfig.glareStrength;
    f64 glareRadius =
        highlightConfig.glareRadiusPixels;

    f64 flareThreshold =
        highlightConfig.flareThreshold;
    f64 flareStrength =
        highlightConfig.flareStrength;
    f64 flareCompactness =
        highlightConfig.flareCompactness;
    f64 flareGhostScale =
        highlightConfig.flareGhostScale;

    highlightChanged |=
        context.InputDouble(
            ("Bloom Threshold##highlight-bloom-threshold-" +
             std::string(viewportId)),
            bloomThreshold);
    highlightChanged |=
        context.InputDouble(
            ("Bloom Soft Knee##highlight-bloom-knee-" +
             std::string(viewportId)),
            bloomKnee);
    highlightChanged |=
        context.InputDouble(
            ("Bloom Strength##highlight-bloom-strength-" +
             std::string(viewportId)),
            bloomStrength);
    highlightChanged |=
        context.InputDouble(
            ("Bloom Radius px##highlight-bloom-radius-" +
             std::string(viewportId)),
            bloomRadius);

    highlightChanged |=
        context.InputDouble(
            ("Glare Threshold##highlight-glare-threshold-" +
             std::string(viewportId)),
            glareThreshold);
    highlightChanged |=
        context.InputDouble(
            ("Glare Strength##highlight-glare-strength-" +
             std::string(viewportId)),
            glareStrength);
    highlightChanged |=
        context.InputDouble(
            ("Glare Radius px##highlight-glare-radius-" +
             std::string(viewportId)),
            glareRadius);

    highlightChanged |=
        context.InputDouble(
            ("Flare Threshold##highlight-flare-threshold-" +
             std::string(viewportId)),
            flareThreshold);
    highlightChanged |=
        context.InputDouble(
            ("Flare Strength##highlight-flare-strength-" +
             std::string(viewportId)),
            flareStrength);
    highlightChanged |=
        context.InputDouble(
            ("Flare Compactness##highlight-flare-compactness-" +
             std::string(viewportId)),
            flareCompactness);
    highlightChanged |=
        context.InputDouble(
            ("Flare Ghost Scale##highlight-flare-ghost-" +
             std::string(viewportId)),
            flareGhostScale);

    highlightConfig.bloomEnabled =
        bloomEnabled;
    highlightConfig.glareEnabled =
        glareEnabled;
    highlightConfig.flareEnabled =
        flareEnabled;
    highlightConfig.bloomThreshold =
        static_cast<f32>(bloomThreshold);
    highlightConfig.bloomKnee =
        static_cast<f32>(bloomKnee);
    highlightConfig.bloomStrength =
        static_cast<f32>(bloomStrength);
    highlightConfig.bloomRadiusPixels =
        static_cast<f32>(bloomRadius);
    highlightConfig.glareThreshold =
        static_cast<f32>(glareThreshold);
    highlightConfig.glareStrength =
        static_cast<f32>(glareStrength);
    highlightConfig.glareRadiusPixels =
        static_cast<f32>(glareRadius);
    highlightConfig.flareThreshold =
        static_cast<f32>(flareThreshold);
    highlightConfig.flareStrength =
        static_cast<f32>(flareStrength);
    highlightConfig.flareCompactness =
        static_cast<f32>(flareCompactness);
    highlightConfig.flareGhostScale =
        static_cast<f32>(flareGhostScale);

    if (context.Button(
            "Composite##highlight-debug-composite-" +
            std::string(viewportId)))
    {
        highlightConfig.debugMode =
            post_process::
                HighlightDebugMode::Composite;
        highlightChanged = true;
    }

    if (context.Button(
            "Bloom Extraction##highlight-debug-bloom-" +
            std::string(viewportId)))
    {
        highlightConfig.debugMode =
            post_process::
                HighlightDebugMode::BloomExtraction;
        highlightChanged = true;
    }

    if (context.Button(
            "Glare Extraction##highlight-debug-glare-" +
            std::string(viewportId)))
    {
        highlightConfig.debugMode =
            post_process::
                HighlightDebugMode::GlareExtraction;
        highlightChanged = true;
    }

    if (context.Button(
            "Flare Extraction##highlight-debug-flare-" +
            std::string(viewportId)))
    {
        highlightConfig.debugMode =
            post_process::
                HighlightDebugMode::FlareExtraction;
        highlightChanged = true;
    }

    if (context.Button(
            "Reset Highlight Defaults##highlight-reset-" +
            std::string(viewportId)))
    {
        highlightConfig = {};
        highlightChanged = true;
    }

    if (highlightChanged)
    {
        renderer_->
            SetHighlightEffectsConfig(
                viewportId,
                highlightConfig);
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
