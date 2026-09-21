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
