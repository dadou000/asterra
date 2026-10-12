#include "StudioViewportInternals.hpp"

namespace orbit::studio_ui
{
using namespace viewport_detail;

namespace
{
// Authored point and spot lights as cloud point lights (ORBIT_CLOUD_POINT_LIGHTS=1). At most kMaxPointLights are used:
// the strongest by flux over squared distance from the camera, with every remaining light folded into one aggregated light
// at their flux-weighted centroid, so no light's energy is dropped. Spot cone shaping is ignored (treated as a point).
// Flux becomes scene-linear radiant intensity: lumens / 683 lm/W = watts, / (4 pi) = W/sr, / 1361 W/m2 = the reference
// irradiance every other stellar term is expressed against.
[[nodiscard]] std::vector<celestial_clouds::CloudRenderer::PointLight> GatherCloudPointLights(
    studio_session::StudioSession& session,
    const std::optional<universe::BodyId>& body,
    const math::Double3& cameraPositionMeters,
    const std::vector<lighting::LocalLight>& transientLights)
{
    using Light = celestial_clouds::CloudRenderer::PointLight;
    struct Source
    {
        f64 flux;
        math::Double3 position;
        math::Double3 color;
        f64 score;
    };
    std::vector<Source> sources;
    const auto add = [&](const f64 flux, const math::Double3& position, const math::Double3& color) {
        if (!(flux > 0.0))
        {
            return;
        }
        const math::Double3 delta = position - cameraPositionMeters;
        sources.push_back({flux, position, color, flux / std::max(math::Dot(delta, delta), 25.0)});
    };
    if (body.has_value())
    {
        const auto root = session.World().Universe().ObjectForBody(*body);
        for (const auto& light : world_model::ResolveAuthoredLocalLights(session.World().Objects(), root))
        {
            add(light.luminousFluxLumens, light.positionMeters, light.colorLinear);
        }
    }
    // Lightning and other transient emitters are ordinary local lights to the cloud march as well.
    for (const auto& light : transientLights)
    {
        add(light.luminousFluxLumens, light.positionInFrameMeters,
            {light.colorLinear.x, light.colorLinear.y, light.colorLinear.z});
    }
    std::vector<Light> result;
    if (sources.empty())
    {
        return result;
    }
    constexpr f64 kLumensPerWatt = 683.0;
    constexpr f64 kFourPi = 12.566370614359172;
    const std::size_t kept = std::min<std::size_t>(sources.size(), celestial_clouds::CloudRenderer::kMaxPointLights - 1U);
    std::partial_sort(sources.begin(), sources.begin() + static_cast<std::ptrdiff_t>(kept), sources.end(),
                      [](const Source& a, const Source& b) { return a.score > b.score; });
    const auto toLight = [&](const f64 flux, const math::Double3& position, const math::Double3& color) {
        const f64 scale = flux / kLumensPerWatt / kFourPi / kStudioReferenceIrradianceWattsPerSquareMeter;
        return Light{position, {static_cast<f32>(color.x * scale), static_cast<f32>(color.y * scale), static_cast<f32>(color.z * scale)}};
    };
    for (std::size_t i = 0; i < kept; ++i)
    {
        result.push_back(toLight(sources[i].flux, sources[i].position, sources[i].color));
    }
    if (sources.size() > kept)
    {
        f64 flux = 0.0;
        math::Double3 centroid{}, color{};
        for (std::size_t i = kept; i < sources.size(); ++i)
        {
            flux += sources[i].flux;
            centroid = centroid + sources[i].position * sources[i].flux;
            color = color + sources[i].color * sources[i].flux;
        }
        result.push_back(toLight(flux, centroid / flux, color / flux));
    }
    return result;
}
} // namespace

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
            // A placed storm domain advances with the simulation clock and replaces the procedural clouds inside it.
            UpdateStormDomain(
                context.atTime,
                cloudFound->second.body,
                cloudReferenceRadius,
                atmosphereView.cameraPositionMeters,
                atmosphereView.sunDirection,
                cloudFound->second.field->layers.front().parameters);
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
            // Split view (clouds.split_set): the left of the viewport is marched with the shipped defines (variant 0), the right
            // with the compare defines (variant 1); each is scissored to its half and resolved against its own history.
            // Temporal reuse of the march (CLOUD_CHECKERBOARD=1/2): the march target is smaller than the cloud size and the
            // resolved history keeps the full size, so the cloud size is made even.
            const u32 variantCount = cloudRenderer_.CompareActive() ? 2U : 1U;
            const f32 splitPosition =
                variantCount == 2U ? std::clamp(cloudRenderer_.ComparePosition(), 0.02F, 0.98F) : 1.0F;
            const bool anyCheckerboard = cloudRenderer_.CheckerboardEnabled() ||
                (variantCount == 2U && cloudRenderer_.CompareCheckerboardMode() != 0U);
            u32 cloudWidth = std::max(
                1U, static_cast<u32>(std::ceil(static_cast<f32>(width) * cloudScale)));
            u32 cloudHeight = std::max(
                1U, static_cast<u32>(std::ceil(static_cast<f32>(height) * cloudScale)));
            if (anyCheckerboard)
            {
                cloudWidth += cloudWidth & 1U;
                cloudHeight += cloudHeight & 1U;
            }
            std::vector<celestial_clouds::CloudRenderer::PointLight> pointLights;
            std::vector<render_graph::TextureHandle> cloudHandles;
            std::vector<render_graph::TextureHandle> resolvedHandles;
            std::vector<rhi::Texture*> resolvedTextures;
            for (u32 variant = 0U; variant < variantCount; ++variant)
            {
            const std::string variantKey = variant == 0U ? info.id : info.id + ".B";
            const std::string variantPrefix = variant == 0U ? prefix : prefix + ".B";
            const u32 checkerboardMode =
                variant == 0U ? cloudRenderer_.CheckerboardMode() : cloudRenderer_.CompareCheckerboardMode();
            const u32 marchWidth = checkerboardMode != 0U ? cloudWidth / 2U : cloudWidth;
            const u32 marchHeight = checkerboardMode == 1U ? cloudHeight / 2U : cloudHeight;
            const f32 regionLeft = variant == 0U ? 0.0F : splitPosition;
            const f32 regionRight = variant == 0U ? splitPosition : 1.0F;
            auto& cloudTarget = cloudTargets_[variantKey];
            if (cloudTarget == nullptr ||
                cloudTarget->Width() != marchWidth ||
                cloudTarget->Height() != marchHeight)
            {
                cloudTarget = device_->CreateTexture({
                    .width = marchWidth,
                    .height = marchHeight,
                    .format = rhi::TextureFormat::RGBA16_Float,
                    .initialState = rhi::ResourceState::ShaderResource});
            }
            auto* cloudTexture = cloudTarget.get();
            const auto cloudHandle = graph.ImportTexture(
                variantPrefix + ".CloudTarget",
                *cloudTexture,
                rhi::ResourceState::ShaderResource);

            // Temporal accumulation: the march is resolved against last frame's
            // result into one of two persistent targets (see CloudRenderer::Resolve).
            auto& temporal = cloudTemporal_[variantKey];
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
            if (temporal.splitPosition != splitPosition)
            {
                // The drawn region moved: the history outside the old one was never marched.
                temporal.valid = false;
                temporal.splitPosition = splitPosition;
            }
            const u32 writeIndex = temporal.writeIndex;
            auto* resolvedTexture = temporal.resolved[writeIndex].get();
            auto* historyTexture = temporal.resolved[1U - writeIndex].get();
            const bool historyValid = temporalOn && temporal.valid;
            const auto previousView = temporal.previous;
            const u32 cloudFrame = temporal.frame++;
            const auto resolvedHandle = graph.ImportTexture(
                variantPrefix + ".CloudResolved",
                *resolvedTexture,
                rhi::ResourceState::ShaderResource);
            const auto historyHandle = graph.ImportTexture(
                variantPrefix + ".CloudHistory",
                *historyTexture,
                rhi::ResourceState::ShaderResource);

            // The sun optical-depth cache update is its own pass so the GPU
            // pass timer reports it separately from the ray march. It declares
            // the same cloud target as the march, which keeps it ordered
            // before it; the recorded commands are unchanged.
            if (variant == 0U)
            {
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
            }

            // Authored lights that scatter into the clouds (ORBIT_CLOUD_POINT_LIGHTS=1, off by default).
            if (variant == 0U && cloudRenderer_.PointLightsEnabled())
            {
                lighting::CameraPlacement flashPlacement;
                flashPlacement.positionMeters = {
                    static_cast<f64>(cloudView.cameraPositionMeters.x),
                    static_cast<f64>(cloudView.cameraPositionMeters.y),
                    static_cast<f64>(cloudView.cameraPositionMeters.z)};
                flashPlacement.forward = {cloudView.forward.x, cloudView.forward.y, cloudView.forward.z};
                flashPlacement.up = {cloudView.up.x, cloudView.up.y, cloudView.up.z};
                const auto flashLights = TransientFlashLights(flashPlacement);
                pointLights = GatherCloudPointLights(
                    context.session,
                    logicalTarget->target.has_value() ? std::optional<universe::BodyId>(logicalTarget->target->body) : std::nullopt,
                    math::Double3{
                        static_cast<f64>(cloudView.cameraPositionMeters.x),
                        static_cast<f64>(cloudView.cameraPositionMeters.y),
                        static_cast<f64>(cloudView.cameraPositionMeters.z)},
                    flashLights);
            }

            cloudHandles.push_back(cloudHandle);
            if (variant == 1U)
            {
                // Orders the second march after the light volume update, which only declares the first cloud target.
                graph.AddPass(
                    variantPrefix + ".CloudsOrder",
                    {
                        {
                            .texture = cloudHandles.front(),
                            .state = rhi::ResourceState::ShaderResource,
                            .access = render_graph::Access::Read
                        },
                        {
                            .texture = cloudHandle,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        }
                    },
                    [](rhi::CommandList&, const render_graph::Resources&) {});
            }
            graph.AddPass(
                variantPrefix + ".Clouds",
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
                 pointLights,
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
                 cloudVolume,
                 variant,
                 regionLeft,
                 regionRight](
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
                        cloudVolumeDebug,
                        pointLights,
                        variant,
                        regionLeft,
                        regionRight);
                });

            graph.AddPass(
                variantPrefix + ".CloudsResolve",
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
                 historyValid,
                 cloudFrame,
                 variant,
                 regionLeft,
                 regionRight](
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
                        0.2F,
                        cloudFrame,
                        variant,
                        regionLeft,
                        regionRight);
                });

            temporal.previous = nowView;
            temporal.previousLab = cloudLab;
            temporal.valid = true;
            temporal.writeIndex = 1U - writeIndex;
            resolvedHandles.push_back(resolvedHandle);
            resolvedTextures.push_back(resolvedTexture);
            }

            if (variantCount == 2U)
            {
                graph.AddPass(
                    prefix + ".CloudsComposite",
                    {
                        {
                            .texture = resolvedHandles[0],
                            .state = rhi::ResourceState::ShaderResource,
                            .access = render_graph::Access::Read
                        },
                        {
                            .texture = resolvedHandles[1],
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
                     left = resolvedTextures[0],
                     right = resolvedTextures[1],
                     color,
                     width,
                     height,
                     splitPosition](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        cloudRenderer_.CompositeSplit(commands, *left, *right, *color, width, height, splitPosition);
                    });
            }
            else
            graph.AddPass(
                prefix + ".CloudsComposite",
                {
                    {
                        .texture = resolvedHandles.front(),
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
                 resolvedTexture = resolvedTextures.front(),
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

            WatchStrikes(atmosphereCamera, width, height);

            // The channels of the live lightning flashes as glowing lines over the clouds (storm.lightning arcs).
            auto arcLines = FlashArcLines(atmosphereCamera, height);
            {
                auto gridLines = ElectrificationGridLines(atmosphereCamera);
                arcLines.insert(arcLines.end(), gridLines.begin(), gridLines.end());
            }
            if (!arcLines.empty())
            {
                graph.AddPass(
                    prefix + ".LightningArcs",
                    {
                        {
                            .texture = targets.color,
                            .state = rhi::ResourceState::RenderTarget,
                            .access = render_graph::Access::Write
                        }
                    },
                    [this, color, width, height, atmosphereCamera, arcLines = std::move(arcLines)](
                        rhi::CommandList& commands,
                        const render_graph::Resources&)
                    {
                        pathRenderer_.DrawCameraRelativeLines(commands, *color, width, height, atmosphereCamera, arcLines);
                    });
            }
        }
    }

}
} // namespace orbit::studio_ui
