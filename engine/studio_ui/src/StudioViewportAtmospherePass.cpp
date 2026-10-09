#include "StudioViewportInternals.hpp"

namespace orbit::studio_ui
{
using namespace viewport_detail;

void StudioViewportRenderer::ComposeAtmospherePasses(StudioLightingPassContext& context)
{
    auto& graph = context.graph;
    auto* view = context.view;
    const auto& info = context.info;
    const auto& targets = context.targets;
    const auto& prefix = context.prefix;
    const auto& studioDirectLight = context.studioDirectLight;
    const auto* logicalTarget = context.session.Viewports().Find(info.id);
    const auto width = view->Width();
    const auto height = view->Height();
    auto* color = &view->Color();
    auto* lightingDepth = &view->Depth();
    // Physical atmosphere: attenuate the lit scene along each view
    // ray and add single + multiple in-scattering, for both the
    // limb seen from orbit and aerial perspective near the ground.
    if (const auto atmosphereFound =
            atmospherePresentations_.find(
                info.id);
        atmosphereFound !=
                atmospherePresentations_.end() &&
            atmosphereFound->second.gpu != nullptr &&
            logicalTarget->target.has_value() &&
            atmosphereFound->second.body ==
                logicalTarget->target->body &&
            studioDirectLight.direct.has_value() &&
            !info.layers.bypassAtmosphere &&
            logicalTarget->mode !=
                studio_session::ViewportMode::Debug)
    {
        auto& scratch =
            atmosphereScratch_[info.id];

        if (scratch == nullptr ||
            scratch->Width() != width ||
            scratch->Height() != height)
        {
            scratch =
                device_->CreateTexture({
                    .width = width,
                    .height = height,
                    .format =
                        rhi::TextureFormat::
                            RGBA16_Float,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
        }

        const auto scratchHandle =
            graph.ImportTexture(
                prefix + ".AtmosphereScratch",
                *scratch,
                rhi::ResourceState::
                    ShaderResource);

        auto* atmosphereScratch =
            scratch.get();
        auto* atmosphereLuts =
            atmosphereFound->second.gpu.get();
        const auto atmosphereParameters =
            atmosphereFound->second.parameters;
        const auto atmosphereCamera =
            view->Camera();

        const celestial_atmosphere::
            AtmosphereRenderView
            atmosphereView{
                .cameraPositionMeters =
                    atmosphereCamera.
                        localPositionMeters,
                .forward =
                    atmosphereCamera.forward,
                .up =
                    atmosphereCamera.up,
                .verticalFovRadians =
                    atmosphereCamera.
                        verticalFovRadians,
                .nearPlaneMeters =
                    atmosphereCamera.
                        nearPlaneMeters,
                .farPlaneMeters =
                    atmosphereCamera.
                        farPlaneMeters,
                .sunDirection =
                    studioDirectLight.
                        directionBody,
                .irradianceScale =
                    studioDirectLight.
                        irradianceScale
            };

        graph.AddPass(
            prefix + ".Atmosphere",
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
                    .texture = targets.depth,
                    .state =
                        rhi::ResourceState::
                            DepthRead,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture = scratchHandle,
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
             lightingDepth,
             atmosphereLuts,
             atmosphereScratch,
             width,
             height,
             atmosphereParameters,
             atmosphereView](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                atmosphereRenderer_.Draw(
                    commands,
                    *color,
                    *lightingDepth,
                    *atmosphereLuts,
                    *atmosphereScratch,
                    width,
                    height,
                    atmosphereParameters,
                    atmosphereView);
            });

        graph.AddPass(
            prefix + ".AtmosphereCopyBack",
            {
                {
                    .texture = scratchHandle,
                    .state =
                        rhi::ResourceState::
                            ShaderResource,
                    .access =
                        render_graph::Access::
                            Read
                },
                {
                    .texture = targets.color,
                    .state =
                        rhi::ResourceState::
                            RenderTarget,
                    .access =
                        render_graph::Access::
                            Write
                }
            },
            [this,
             atmosphereScratch,
             color,
             width,
             height](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                debugComposite_.Draw(
                    commands,
                    *atmosphereScratch,
                    *color,
                    width,
                    height);
            });

        // Not drawing clouds this frame leaves the history stale.
        if (!info.layers.clouds)
        {
            if (const auto stale = cloudTemporal_.find(info.id);
                stale != cloudTemporal_.end())
            {
                stale->second.valid = false;
            }
        }

        // Cloud shell in the near-field/clipmap views, composited AFTER the atmosphere pass:
        // the clouds sit inside the atmosphere, so the atmosphere pass must not treat them as
        // surfaces at the terrain depth. The cloud shader applies its own aerial perspective
        // over the camera-to-cloud distance.
        if (const auto cloudFound = cloudPresentations_.find(info.id);
            info.layers.clouds &&
            (info.layers.fullClipmap || !info.layers.macroGlobe) &&
            cloudFound != cloudPresentations_.end() &&
            cloudFound->second.gpu != nullptr &&
            cloudFound->second.field != nullptr &&
            !cloudFound->second.field->layers.empty() &&
            cloudFound->second.body == logicalTarget->target->body)
        {
            auto* cloudGpu = cloudFound->second.gpu.get();
            const auto cloudLayer =
                cloudFound->second.field->layers.front().parameters;
            const f64 cloudReferenceRadius =
                atmosphereFound->second.parameters.bottomRadiusMeters;
            // Cloud lab: one isolated cloud, optionally lit by a chosen sun.
            auto cloudView = atmosphereView;
            math::Float3 labSun = cloudView.sunDirection;
            bool labSunOverridden = false;
            const auto cloudLab = ResolveCloudLab(
                cloudLabAnchors_[info.id],
                info.layers.cloudLab,
                atmosphereCamera,
                cloudReferenceRadius,
                labSun,
                labSunOverridden);
            if (labSunOverridden)
            {
                cloudView.sunDirection = labSun;
            }

            // Half-resolution march, then a depth-bounded bilinear composite.
            const f32 cloudScale =
                std::clamp(info.layers.cloudResolutionScale, 0.25F, 1.0F);
            const f32 cloudGodrays =
                std::clamp(info.layers.cloudGodrayStrength, 0.0F, 2.0F);
            const bool cloudLightVolume = info.layers.cloudLightVolume;
            auto& cloudVolumeSlot = cloudLightVolumes_[info.id];
            if (cloudVolumeSlot == nullptr)
            {
                cloudVolumeSlot = cloudRenderer_.CreateLightVolume();
            }
            auto* cloudVolume = cloudVolumeSlot.get();
            const f32 cloudVolumeDebug = info.layers.cloudVolumeDebugAltitude;
            const u32 cloudWidth = std::max(
                1U, static_cast<u32>(std::ceil(static_cast<f32>(width) * cloudScale)));
            const u32 cloudHeight = std::max(
                1U, static_cast<u32>(std::ceil(static_cast<f32>(height) * cloudScale)));
            auto& cloudTarget = cloudTargets_[info.id];
            if (cloudTarget == nullptr ||
                cloudTarget->Width() != cloudWidth ||
                cloudTarget->Height() != cloudHeight)
            {
                cloudTarget = device_->CreateTexture({
                    .width = cloudWidth,
                    .height = cloudHeight,
                    .format = rhi::TextureFormat::RGBA16_Float,
                    .initialState = rhi::ResourceState::ShaderResource});
            }
            auto* cloudTexture = cloudTarget.get();
            const auto cloudHandle = graph.ImportTexture(
                prefix + ".CloudTarget",
                *cloudTexture,
                rhi::ResourceState::ShaderResource);

            // Temporal accumulation: the march is resolved against last frame's
            // result into one of two persistent targets (see CloudRenderer::Resolve).
            auto& temporal = cloudTemporal_[info.id];
            const bool temporalOn = info.layers.cloudTemporal;
            if (temporal.resolved[0] == nullptr ||
                temporal.resolved[0]->Width() != cloudWidth ||
                temporal.resolved[0]->Height() != cloudHeight)
            {
                for (auto& resolved : temporal.resolved)
                {
                    resolved = device_->CreateTexture({
                        .width = cloudWidth,
                        .height = cloudHeight,
                        .format = rhi::TextureFormat::RGBA16_Float,
                        .initialState = rhi::ResourceState::ShaderResource});
                }
                temporal.valid = false;
            }
            const celestial_clouds::CloudRenderer::CloudResolveView nowView{
                .cameraPositionMeters = cloudView.cameraPositionMeters,
                .forward = cloudView.forward,
                .up = cloudView.up,
                .verticalFovRadians = cloudView.verticalFovRadians,
                .aspect = static_cast<f32>(cloudWidth) / static_cast<f32>(cloudHeight)};
            const auto sameLab =
                [](const celestial_clouds::CloudLab& a, const celestial_clouds::CloudLab& b)
            {
                return a.enabled == b.enabled &&
                    a.centerDirection.x == b.centerDirection.x &&
                    a.centerDirection.y == b.centerDirection.y &&
                    a.centerDirection.z == b.centerDirection.z &&
                    a.radiusMeters == b.radiusMeters && a.type == b.type &&
                    a.coverage == b.coverage && a.cirrus == b.cirrus &&
                    a.precipitation == b.precipitation &&
                    a.heightScale == b.heightScale && a.maturity == b.maturity &&
                    a.organisation == b.organisation && a.density == b.density &&
                    a.cirrusSheet == b.cirrusSheet &&
                    a.seed == b.seed;
            };
            if (!sameLab(temporal.previousLab, cloudLab))
            {
                // The cloud changed under the history (lab placed or edited).
                temporal.valid = false;
            }
            const u32 writeIndex = temporal.writeIndex;
            auto* resolvedTexture = temporal.resolved[writeIndex].get();
            auto* historyTexture = temporal.resolved[1U - writeIndex].get();
            const bool historyValid = temporalOn && temporal.valid;
            const auto previousView = temporal.previous;
            const u32 cloudFrame = temporal.frame++;
            const auto resolvedHandle = graph.ImportTexture(
                prefix + ".CloudResolved",
                *resolvedTexture,
                rhi::ResourceState::ShaderResource);
            const auto historyHandle = graph.ImportTexture(
                prefix + ".CloudHistory",
                *historyTexture,
                rhi::ResourceState::ShaderResource);

            // The sun optical-depth cache update is its own pass so the GPU
            // pass timer reports it separately from the ray march. It declares
            // the same cloud target as the march, which keeps it ordered
            // before it; the recorded commands are unchanged.
            graph.AddPass(
                prefix + ".CloudLightVolume",
                {
                    {
                        .texture = cloudHandle,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 cloudGpu,
                 cloudLayer,
                 cloudReferenceRadius,
                 atmosphereParameters,
                 cloudView,
                 cloudLab,
                 cloudFrame,
                 cloudLightVolume,
                 cloudVolume](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    cloudRenderer_.UpdateLightVolume(
                        commands,
                        *cloudVolume,
                        *cloudGpu,
                        cloudReferenceRadius,
                        cloudLayer,
                        atmosphereParameters,
                        cloudView,
                        cloudLab,
                        cloudFrame,
                        cloudLightVolume);
                });

            graph.AddPass(
                prefix + ".Clouds",
                {
                    {
                        .texture = targets.depth,
                        .state = rhi::ResourceState::DepthRead,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = cloudHandle,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 lightingDepth,
                 atmosphereLuts,
                 cloudTexture,
                 cloudGpu,
                 cloudLayer,
                 cloudReferenceRadius,
                 cloudWidth,
                 cloudHeight,
                 atmosphereParameters,
                 cloudView,
                 cloudLab,
                 cloudFrame,
                 cloudGodrays,
                 cloudVolumeDebug,
                 cloudVolume](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    cloudRenderer_.Draw(
                        commands,
                        *cloudVolume,
                        *lightingDepth,
                        *atmosphereLuts,
                        *cloudGpu,
                        *cloudTexture,
                        cloudWidth,
                        cloudHeight,
                        cloudReferenceRadius,
                        cloudLayer,
                        atmosphereParameters,
                        cloudView,
                        cloudLab,
                        cloudFrame,
                        cloudGodrays,
                        cloudVolumeDebug);
                });

            graph.AddPass(
                prefix + ".CloudsResolve",
                {
                    {
                        .texture = cloudHandle,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = historyHandle,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = resolvedHandle,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 cloudTexture,
                 historyTexture,
                 resolvedTexture,
                 cloudWidth,
                 cloudHeight,
                 cloudReferenceRadius,
                 cloudLayer,
                 nowView,
                 previousView,
                 historyValid](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    // A fifth of the result comes from the new march: about five frames
                    // of averaging, quick enough to follow weather and motion.
                    cloudRenderer_.Resolve(
                        commands,
                        *cloudTexture,
                        *historyTexture,
                        *resolvedTexture,
                        cloudWidth,
                        cloudHeight,
                        cloudReferenceRadius +
                            0.5 * (cloudLayer.baseAltitudeMeters +
                                   cloudLayer.topAltitudeMeters),
                        nowView,
                        previousView,
                        historyValid,
                        0.2F);
                });

            temporal.previous = nowView;
            temporal.previousLab = cloudLab;
            temporal.valid = true;
            temporal.writeIndex = 1U - writeIndex;

            graph.AddPass(
                prefix + ".CloudsComposite",
                {
                    {
                        .texture = resolvedHandle,
                        .state = rhi::ResourceState::ShaderResource,
                        .access = render_graph::Access::Read
                    },
                    {
                        .texture = targets.color,
                        .state = rhi::ResourceState::RenderTarget,
                        .access = render_graph::Access::Write
                    }
                },
                [this,
                 resolvedTexture,
                 color,
                 width,
                 height](
                    rhi::CommandList& commands,
                    const render_graph::Resources&)
                {
                    cloudRenderer_.Composite(
                        commands,
                        *resolvedTexture,
                        *color,
                        width,
                        height);
                });
        }
    }

}
} // namespace orbit::studio_ui
