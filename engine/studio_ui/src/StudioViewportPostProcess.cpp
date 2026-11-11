#include "StudioViewportInternals.hpp"

namespace orbit::studio_ui
{
using namespace viewport_detail;

void StudioViewportRenderer::ComposePostProcess(
        render_graph::RenderGraph& graph,
        render_view::RenderView* view,
        const StudioRenderViewInfo& info,
        const render_view::ImportedTargets& targets,
        const std::string& prefix,
        u32 frameIndex,
        post_process::AntiAliasingMode antiAliasingMode)
{
    const auto width = view->Width();
    const auto height = view->Height();
    auto* color = &view->Color();
    // Anti-aliasing of the HDR scene colour, after every scene pass and
    // before exposure / tone mapping. TAA reprojects its history with the
    // (jittered) camera pair and depth; frames without usable history use
    // FXAA instead.
    if (antiAliasingMode != post_process::AntiAliasingMode::Off &&
        view->SurfaceDebugMode() == lighting::SurfaceDebugMode::Lit &&
        width > 0U && height > 0U)
    {
        auto& aa = antiAliasingPresentations_[info.id];

        if (aa.scratch == nullptr || aa.width != width ||
            aa.height != height)
        {
            const auto make = [&]
            {
                return device_->CreateTexture({
                    .width = width,
                    .height = height,
                    .format = rhi::TextureFormat::RGBA16_Float,
                    .initialState = rhi::ResourceState::ShaderResource,
                    .allowUnorderedAccess = true});
            };
            aa.scratch = make();
            aa.history[0] = make();
            aa.history[1] = make();
            aa.width = width;
            aa.height = height;
            aa.hasHistory = false;
            aa.readIndex = 0U;
        }

        // Reproject with the UN-jittered cameras: the history then stays
        // in a fixed screen grid (a static scene reads the same history
        // pixel every frame) and the jitter only changes what each frame
        // samples. Reprojecting with the jittered pair would resample the
        // history at a different sub-pixel phase each frame and shimmer.
        const auto& camera = aa.base;
        const post_process::TaaCamera currentCamera{
            .positionMeters = camera.localPositionMeters,
            .forward = camera.forward,
            .up = camera.up,
            .verticalFovRadians = camera.verticalFovRadians,
            .nearPlaneMeters = camera.nearPlaneMeters,
            .farPlaneMeters = camera.farPlaneMeters};

        const auto useDepth = render_graph::TextureUse{
            .texture = targets.depth,
            .state = rhi::ResourceState::DepthRead,
            .access = render_graph::Access::Read};
        const auto colorRead = render_graph::TextureUse{
            .texture = targets.color,
            .state = rhi::ResourceState::ShaderResource,
            .access = render_graph::Access::Read};
        const auto colorWrite = render_graph::TextureUse{
            .texture = targets.color,
            .state = rhi::ResourceState::RenderTarget,
            .access = render_graph::Access::Write};

        if (antiAliasingMode == post_process::AntiAliasingMode::Taa)
        {
            const u32 readIndex = aa.readIndex;
            const u32 writeIndex = 1U - readIndex;
            const bool historyValid =
                aa.hasHistory &&
                post_process::TaaHistoryUsable(
                    aa.previousCamera, currentCamera);

            const auto historyReadHandle = graph.ImportTexture(
                prefix + ".TaaHistoryRead",
                *aa.history[readIndex],
                rhi::ResourceState::ShaderResource);
            const auto historyWriteHandle = graph.ImportTexture(
                prefix + ".TaaHistoryWrite",
                *aa.history[writeIndex],
                rhi::ResourceState::ShaderResource);

            if (historyValid)
            {
                graph.AddPass(
                    prefix + ".TaaResolve",
                    {
                        colorRead,
                        useDepth,
                        {.texture = historyReadHandle,
                         .state = rhi::ResourceState::ShaderResource,
                         .access = render_graph::Access::Read},
                        {.texture = historyWriteHandle,
                         .state = rhi::ResourceState::UnorderedAccess,
                         .access = render_graph::Access::Write}
                    },
                    [this,
                     color,
                     depthTexture = &view->Depth(),
                     historyRead = aa.history[readIndex].get(),
                     historyWrite = aa.history[writeIndex].get(),
                     width,
                     height,
                     currentCamera,
                     previousCamera = aa.previousCamera](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        antiAliasingRenderer_.Taa(
                            commands,
                            *color,
                            *depthTexture,
                            *historyRead,
                            *historyWrite,
                            width,
                            height,
                            currentCamera,
                            previousCamera,
                            true);
                    });
            }
            else
            {
                graph.AddPass(
                    prefix + ".TaaFallbackFxaa",
                    {
                        colorRead,
                        {.texture = historyWriteHandle,
                         .state = rhi::ResourceState::UnorderedAccess,
                         .access = render_graph::Access::Write}
                    },
                    [this,
                     color,
                     historyWrite = aa.history[writeIndex].get(),
                     width,
                     height](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        antiAliasingRenderer_.Fxaa(
                            commands, *color, *historyWrite, width, height);
                    });
            }

            graph.AddPass(
                prefix + ".TaaCopyBack",
                {
                    {.texture = historyWriteHandle,
                     .state = rhi::ResourceState::ShaderResource,
                     .access = render_graph::Access::Read},
                    colorWrite
                },
                [this,
                 color,
                 historyWrite = aa.history[writeIndex].get(),
                 width,
                 height](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    debugComposite_.Draw(
                        commands, *historyWrite, *color, width, height);
                });

            aa.readIndex = writeIndex;
            aa.hasHistory = true;
            aa.previousCamera = currentCamera;
        }
        else
        {
            const auto scratchHandle = graph.ImportTexture(
                prefix + ".FxaaScratch",
                *aa.scratch,
                rhi::ResourceState::ShaderResource);

            graph.AddPass(
                prefix + ".FxaaResolve",
                {
                    colorRead,
                    {.texture = scratchHandle,
                     .state = rhi::ResourceState::UnorderedAccess,
                     .access = render_graph::Access::Write}
                },
                [this,
                 color,
                 scratch = aa.scratch.get(),
                 width,
                 height](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    antiAliasingRenderer_.Fxaa(
                        commands, *color, *scratch, width, height);
                });

            graph.AddPass(
                prefix + ".FxaaCopyBack",
                {
                    {.texture = scratchHandle,
                     .state = rhi::ResourceState::ShaderResource,
                     .access = render_graph::Access::Read},
                    colorWrite
                },
                [this,
                 color,
                 scratch = aa.scratch.get(),
                 width,
                 height](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    debugComposite_.Draw(
                        commands, *scratch, *color, width, height);
                });

            aa.hasHistory = false;
        }
    }

    {
        auto& histogram =
            luminanceHistogramPresentations_[
                info.id];

        const bool recreateHistogram =
            histogram.width != width ||
            histogram.height != height ||
            histogram.meteringMask == nullptr ||
            histogram.histogramReadback.size() !=
                framesInFlight_ ||
            histogram.statisticsReadback.size() !=
                framesInFlight_;

        if (recreateHistogram)
        {
            const auto retainedConfig =
                histogram.diagnostics.config;
            const auto retainedEyeConfig =
                histogram.diagnostics.eyeConfig;
            const auto retainedEyeState =
                histogram.diagnostics.eyeState;
            const auto retainedHighlightConfig =
                histogram.diagnostics.highlightConfig;
            const auto retainedToneMapping =
                histogram.diagnostics.toneMapping;
            const auto retainedEyeUpdate =
                histogram.lastEyeUpdate;
            const bool retainedHasEyeUpdateTime =
                histogram.hasEyeUpdateTime;
            const bool retainedOverlay =
                histogram.showMeteringOverlay;

            histogram = {};
            histogram.diagnostics.config =
                retainedConfig;
            histogram.diagnostics.eyeConfig =
                retainedEyeConfig;
            histogram.diagnostics.eyeState =
                retainedEyeState;
            histogram.diagnostics.highlightConfig =
                retainedHighlightConfig;
            histogram.diagnostics.toneMapping =
                retainedToneMapping;
            histogram.lastEyeUpdate =
                retainedEyeUpdate;
            histogram.hasEyeUpdateTime =
                retainedHasEyeUpdateTime;
            histogram.showMeteringOverlay =
                retainedOverlay;
            histogram.width = width;
            histogram.height = height;

            histogram.meteringMask =
                device_->CreateTexture({
                    .width = width,
                    .height = height,
                    .format =
                        rhi::TextureFormat::
                            RGBA16_Float,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource,
                    .allowUnorderedAccess =
                        true
                });

            histogram.histogramReadback.reserve(
                framesInFlight_);
            histogram.statisticsReadback.reserve(
                framesInFlight_);
            histogram.submitted.assign(
                framesInFlight_,
                false);

            for (u32 slot = 0U;
                 slot < framesInFlight_;
                 ++slot)
            {
                histogram.histogramReadback.push_back(
                    device_->CreateBuffer({
                        .sizeBytes =
                            static_cast<u64>(
                                post_process::
                                    kLuminanceHistogramBins) *
                            sizeof(u32),
                        .usage =
                            rhi::BufferUsage::
                                Structured,
                        .memory =
                            rhi::MemoryUsage::
                                HostReadback,
                        .initialState =
                            rhi::ResourceState::
                                CopyDestination
                    }));

                histogram.statisticsReadback.push_back(
                    device_->CreateBuffer({
                        .sizeBytes =
                            sizeof(
                                post_process::
                                    GpuLuminanceHistogramStatistics),
                        .usage =
                            rhi::BufferUsage::
                                Structured,
                        .memory =
                            rhi::MemoryUsage::
                                HostReadback,
                        .initialState =
                            rhi::ResourceState::
                                CopyDestination
                    }));
            }
        }

        const u32 histogramFrameSlot =
            frameIndex %
            framesInFlight_;

        if (histogram.submitted[
                histogramFrameSlot])
        {
            auto* statisticsBytes =
                histogram.statisticsReadback[
                    histogramFrameSlot]->Map();

            post_process::
                GpuLuminanceHistogramStatistics
                    gpuStatistics{};

            std::memcpy(
                &gpuStatistics,
                statisticsBytes,
                sizeof(gpuStatistics));

            histogram.statisticsReadback[
                histogramFrameSlot]->Unmap();

            auto* histogramBytes =
                histogram.histogramReadback[
                    histogramFrameSlot]->Map();

            std::memcpy(
                histogram.diagnostics.bins.data(),
                histogramBytes,
                static_cast<std::size_t>(
                    post_process::
                        kLuminanceHistogramBins) *
                    sizeof(u32));

            histogram.histogramReadback[
                histogramFrameSlot]->Unmap();

            histogram.diagnostics.statistics =
                post_process::
                    DecodeLuminanceHistogramStatistics(
                        gpuStatistics);

            const auto eyeNow =
                std::chrono::steady_clock::now();

            f32 eyeDeltaSeconds =
                1.0F / 60.0F;

            if (histogram.hasEyeUpdateTime)
            {
                eyeDeltaSeconds =
                    std::clamp(
                        std::chrono::duration<f32>(
                            eyeNow -
                            histogram.lastEyeUpdate).
                            count(),
                        1.0F / 240.0F,
                        0.25F);
            }

            if (!histogram.diagnostics.eyeAdaptationLocked)
            {
                // The display's reference white and peak luminance (nits)
                // are the tone-mapping config's: the eye protects the
                // highlights against what that display can actually show.
                auto eyeConfig =
                    histogram.diagnostics.eyeConfig;
                eyeConfig.referenceWhiteNits =
                    histogram.diagnostics.toneMapping.
                        referenceWhiteNits;
                eyeConfig.highlightTargetNits =
                    histogram.diagnostics.toneMapping.
                        peakNits;

                histogram.diagnostics.eyeState =
                    post_process::
                        UpdateHumanEyeAdaptation(
                            histogram.diagnostics.eyeState,
                            histogram.diagnostics.statistics,
                            eyeDeltaSeconds,
                            eyeConfig);
            }

            histogram.lastEyeUpdate =
                eyeNow;
            histogram.hasEyeUpdateTime =
                true;

            histogram.diagnostics.
                meteringMaskAvailable =
                    true;
        }

        const auto histogramHandle =
            graph.CreateBuffer(
                prefix +
                    ".LuminanceHistogram",
                {
                    .sizeBytes =
                        static_cast<u64>(
                            post_process::
                                kLuminanceHistogramBins) *
                        sizeof(u32),
                    .usage =
                        rhi::BufferUsage::
                            Structured,
                    .memory =
                        rhi::MemoryUsage::
                            GpuOnly,
                    .initialState =
                        rhi::ResourceState::
                            UnorderedAccess
                });

        const auto histogramStatisticsHandle =
            graph.CreateBuffer(
                prefix +
                    ".LuminanceStatistics",
                {
                    .sizeBytes =
                        sizeof(
                            post_process::
                                GpuLuminanceHistogramStatistics),
                    .usage =
                        rhi::BufferUsage::
                            Structured,
                    .memory =
                        rhi::MemoryUsage::
                            GpuOnly,
                    .initialState =
                        rhi::ResourceState::
                            UnorderedAccess
                });

        const auto histogramMaskHandle =
            graph.ImportTexture(
                prefix +
                    ".LuminanceMeteringMask",
                *histogram.meteringMask,
                rhi::ResourceState::
                    ShaderResource);

        const auto histogramReadbackHandle =
            graph.ImportBuffer(
                prefix +
                    ".LuminanceHistogramReadback",
                *histogram.histogramReadback[
                    histogramFrameSlot],
                rhi::ResourceState::
                    CopyDestination);

        const auto statisticsReadbackHandle =
            graph.ImportBuffer(
                prefix +
                    ".LuminanceStatisticsReadback",
                *histogram.statisticsReadback[
                    histogramFrameSlot],
                rhi::ResourceState::
                    CopyDestination);

        const auto histogramConfig =
            histogram.diagnostics.config;

        graph.AddPass(
            prefix +
                ".LuminanceHistogramReset",
            {},
            {
                {
                    .buffer =
                        histogramHandle,
                    .state =
                        rhi::ResourceState::
                            UnorderedAccess,
                    .access =
                        render_graph::Access::
                            Write
                },
                {
                    .buffer =
                        histogramStatisticsHandle,
                    .state =
                        rhi::ResourceState::
                            UnorderedAccess,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            [this,
             histogramHandle,
             histogramStatisticsHandle](
                rhi::CommandList& commands,
                const render_graph::Resources&
                    resources)
            {
                luminanceHistogramRenderer_.
                    Reset(
                        commands,
                        resources.Buffer(
                            histogramHandle),
                        resources.Buffer(
                            histogramStatisticsHandle));
            });

        graph.AddPass(
            prefix +
                ".LuminanceHistogramBuild",
            {
                {
                    .texture =
                        targets.color,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture =
                        histogramMaskHandle,
                    .state =
                        rhi::ResourceState::
                            UnorderedAccess,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            {
                {
                    .buffer =
                        histogramHandle,
                    .state =
                        rhi::ResourceState::
                            UnorderedAccess,
                    .access =
                        render_graph::Access::
                            Write
                },
                {
                    .buffer =
                        histogramStatisticsHandle,
                    .state =
                        rhi::ResourceState::
                            UnorderedAccess,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            [this,
             color,
             histogramMask =
                histogram.meteringMask.get(),
             histogramHandle,
             histogramStatisticsHandle,
             width,
             height,
             histogramConfig](
                rhi::CommandList& commands,
                const render_graph::Resources&
                    resources)
            {
                luminanceHistogramRenderer_.
                    Build(
                        commands,
                        *color,
                        *histogramMask,
                        resources.Buffer(
                            histogramHandle),
                        resources.Buffer(
                            histogramStatisticsHandle),
                        width,
                        height,
                        histogramConfig);
            });

        graph.AddPass(
            prefix +
                ".LuminanceHistogramReduce",
            {},
            {
                {
                    .buffer =
                        histogramHandle,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .buffer =
                        histogramStatisticsHandle,
                    .state =
                        rhi::ResourceState::
                            UnorderedAccess,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            [this,
             histogramHandle,
             histogramStatisticsHandle,
             histogramConfig](
                rhi::CommandList& commands,
                const render_graph::Resources&
                    resources)
            {
                luminanceHistogramRenderer_.
                    Reduce(
                        commands,
                        resources.Buffer(
                            histogramHandle),
                        resources.Buffer(
                            histogramStatisticsHandle),
                        histogramConfig);
            });

        graph.AddPass(
            prefix +
                ".LuminanceHistogramReadback",
            {},
            {
                {
                    .buffer =
                        histogramHandle,
                    .state =
                        rhi::ResourceState::
                            CopySource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .buffer =
                        histogramStatisticsHandle,
                    .state =
                        rhi::ResourceState::
                            CopySource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .buffer =
                        histogramReadbackHandle,
                    .state =
                        rhi::ResourceState::
                            CopyDestination,
                    .access =
                        render_graph::Access::
                            Write
                },
                {
                    .buffer =
                        statisticsReadbackHandle,
                    .state =
                        rhi::ResourceState::
                            CopyDestination,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            [histogramHandle,
             histogramStatisticsHandle,
             histogramReadbackHandle,
             statisticsReadbackHandle](
                rhi::CommandList& commands,
                const render_graph::Resources&
                    resources)
            {
                commands.CopyBuffer(
                    resources.Buffer(
                        histogramHandle),
                    0U,
                    resources.Buffer(
                        histogramReadbackHandle),
                    0U,
                    static_cast<u64>(
                        post_process::
                            kLuminanceHistogramBins) *
                        sizeof(u32));

                commands.CopyBuffer(
                    resources.Buffer(
                        histogramStatisticsHandle),
                    0U,
                    resources.Buffer(
                        statisticsReadbackHandle),
                    0U,
                    sizeof(
                        post_process::
                            GpuLuminanceHistogramStatistics));
            });

        graph.AddPass(
            prefix +
                ".LuminanceMeteringMaskRestore",
            {
                {
                    .texture =
                        histogramMaskHandle,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                }
            },
            [](
                rhi::CommandList&,
                const render_graph::Resources&)
            {
            });

        histogram.submitted[
            histogramFrameSlot] =
                true;
    }

    auto& histogram =
        luminanceHistogramPresentations_[
            info.id];

    if (colorLut_ == nullptr)
    {
        throw std::logic_error(
            "Studio viewport LUT correction has no GPU LUT.");
    }

    auto* displayLinear =
        &view->DisplayLinear();
    auto* displayGraded =
        &view->DisplayGraded();
    auto* displayColor =
        &view->DisplayColor();
    auto* colorLut =
        colorLut_.get();
    auto displayResolveSettings =
        displayResolveSettings_;

    // M25: eye adaptation controls presentation exposure only. The HDR
    // scene target remains physically untouched for GI, histogram,
    // bloom/glare extraction and future HDR output.
    if (histogram.diagnostics.eyeState.initialized)
    {
        displayResolveSettings.exposureScale *=
            histogram.diagnostics.eyeState.exposureScale;
    }

    auto colorLutSettings =
        colorLutSettings_;

    const auto surfaceDebugMode =
        view->SurfaceDebugMode();

    if (surfaceDebugMode !=
        lighting::SurfaceDebugMode::Lit)
    {
        // Diagnostic colors are data visualization, not presentation.
        // Do not let a user grade/LUT disguise the underlying buffers.
        colorLutSettings.enabled = false;
    }

    if (surfaceDebugMode ==
        lighting::SurfaceDebugMode::Lit)
    {
        graph.AddPass(
            prefix + ".DisplayResolve",
            {
                {
                    .texture = targets.color,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture = targets.displayLinear,
                    .state =
                        rhi::ResourceState::
                            RenderTarget,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            [this,
             color,
             displayLinear,
             width,
             height,
             displayResolveSettings,
             infoId = info.id](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                const auto found =
                    luminanceHistogramPresentations_.find(
                        infoId);

                if (found ==
                    luminanceHistogramPresentations_.end())
                {
                    displayResolveRenderer_.Draw(
                        commands,
                        *color,
                        *displayLinear,
                        width,
                        height,
                        displayResolveSettings);
                    return;
                }

                auto highlightConfig =
                    found->second.diagnostics.highlightConfig;

                // M25 supplies scene-level evidence that strong highlight
                // effects are warranted. Bloom remains a local soft-knee
                // optical response; glare requires upper-percentile
                // excess and flare requires an extreme peak.
                if (found->second.diagnostics.
                        eyeState.p99ExcessStops <= 0.0F)
                {
                    highlightConfig.glareEnabled = false;
                }

                if (found->second.diagnostics.
                        eyeState.peakExcessStops <= 0.0F)
                {
                    highlightConfig.flareEnabled = false;
                }

                highlightEffectsRenderer_.Draw(
                    commands,
                    *color,
                    *displayLinear,
                    width,
                    height,
                    displayResolveSettings.exposureScale,
                    found->second.diagnostics.toneMapping,
                    highlightConfig);
            });
    }
    else
    {
        rhi::Texture* surfaceDebugSource = nullptr;
        render_graph::TextureHandle surfaceDebugHandle{};

        switch (surfaceDebugMode)
        {
        case lighting::SurfaceDebugMode::BaseColorRoughness:
            surfaceDebugSource =
                &view->SurfaceBaseRoughness();
            surfaceDebugHandle =
                targets.surfaceBaseRoughness;
            break;

        case lighting::SurfaceDebugMode::NormalMetallic:
            surfaceDebugSource =
                &view->SurfaceNormalMetallic();
            surfaceDebugHandle =
                targets.surfaceNormalMetallic;
            break;

        case lighting::SurfaceDebugMode::EmissionMetadata:
            surfaceDebugSource =
                &view->SurfaceEmissionClass();
            surfaceDebugHandle =
                targets.surfaceEmissionClass;
            break;

        case lighting::SurfaceDebugMode::Lit:
            break;
        }

        if (surfaceDebugSource == nullptr)
        {
            throw std::logic_error(
                "Studio surface debug mode has no source attachment.");
        }

        graph.AddPass(
            prefix + ".SurfaceDebugResolve",
            {
                {
                    .texture = surfaceDebugHandle,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture = targets.displayLinear,
                    .state =
                        rhi::ResourceState::
                            RenderTarget,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            [this,
             surfaceDebugSource,
             displayLinear,
             width,
             height,
             surfaceDebugMode](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                surfaceDebugRenderer_.Draw(
                    commands,
                    *surfaceDebugSource,
                    *displayLinear,
                    width,
                    height,
                    surfaceDebugMode);
            });
    }

    graph.AddPass(
        prefix + ".ColorLutCorrection",
        {
            {
                .texture = targets.displayLinear,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture = targets.displayGraded,
                .state =
                    rhi::ResourceState::
                        RenderTarget,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        [this,
         displayLinear,
         displayGraded,
         width,
         height,
         colorLut,
         colorLutSettings](
            rhi::CommandList& commands,
            const render_graph::Resources&)
        {
            colorLutRenderer_.Draw(
                commands,
                *displayLinear,
                *displayGraded,
                width,
                height,
                *colorLut,
                colorLutSettings);
        });

    const auto outputDiagnostics =
        post_process::
            ResolveOutputTransform(
                outputTransformSettings_,
                outputDisplayCapabilities_);

    graph.AddPass(
        prefix + ".OutputTransform",
        {
            {
                .texture = targets.displayGraded,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture = targets.display,
                .state =
                    rhi::ResourceState::
                        RenderTarget,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        [this,
         displayGraded,
         displayColor,
         width,
         height,
         outputDiagnostics,
         outputPattern =
             outputTransformSettings_.
                 testPattern](
            rhi::CommandList& commands,
            const render_graph::Resources&)
        {
            outputTransformRenderer_.Draw(
                commands,
                *displayGraded,
                *displayColor,
                width,
                height,
                outputDiagnostics,
                outputPattern);
        });

    if (histogram.showMeteringOverlay &&
        histogram.meteringMask != nullptr)
    {
        const auto histogramMaskHandle =
            graph.ImportTexture(
                prefix +
                    ".LuminanceMeteringOverlayMask",
                *histogram.meteringMask,
                rhi::ResourceState::
                    ShaderResource);

        graph.AddPass(
            prefix +
                ".LuminanceMeteringOverlay",
            {
                {
                    .texture =
                        histogramMaskHandle,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture =
                        targets.display,
                    .state =
                        rhi::ResourceState::
                            RenderTarget,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            [this,
             mask =
                histogram.meteringMask.get(),
             displayColor,
             width,
             height](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                luminanceHistogramRenderer_.
                    DrawMeteringOverlay(
                        commands,
                        *mask,
                        *displayColor,
                        width,
                        height);
            });

        graph.AddPass(
            prefix +
                ".LuminanceMeteringOverlayRestore",
            {
                {
                    .texture =
                        targets.display,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                }
            },
            [](
                rhi::CommandList&,
                const render_graph::Resources&)
            {
            });
    }

}
} // namespace orbit::studio_ui
