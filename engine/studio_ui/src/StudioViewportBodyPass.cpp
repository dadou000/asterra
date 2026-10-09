#include "StudioViewportInternals.hpp"

namespace orbit::studio_ui
{
using namespace viewport_detail;

void StudioViewportRenderer::ComposeBodyPreviewPass(StudioScenePassContext& context)
{
    auto& graph = context.graph;
    auto& session = context.session;
    auto* view = context.view;
    const auto& info = context.info;
    const auto& targets = context.targets;
    const auto& prefix = context.prefix;
    const auto& studioDirectLight = context.studioDirectLight;
    const auto& resolvedOceanForView = context.resolvedOceanForView;
    const auto& shape = context.shape;
    const auto& acquireCelestialGrant = context.acquireCelestialGrant;
    const auto& completeCelestialGrant = context.completeCelestialGrant;
    const auto& drawBackgroundBodies = context.drawBackgroundBodies;
    const auto width = view->Width();
    const auto height = view->Height();
    auto* color = &view->Color();
    const auto* logicalTarget = session.Viewports().Find(info.id);

            macroGlobePresentations_.erase(
                info.id);
            terrainPresentations_.erase(
                info.id);

            const auto camera =
                view->Camera();
            const auto bodyShape =
                *shape;
            auto* bodySurfaceBaseRoughness =
                &view->SurfaceBaseRoughness();
            auto* bodySurfaceNormalMetallic =
                &view->SurfaceNormalMetallic();
            auto* bodySurfaceEmissionClass =
                &view->SurfaceEmissionClass();

    const bool perspective =
        logicalTarget->mode ==
        studio_session::
            ViewportMode::Perspective;

    const auto compactBodyObject =
        logicalTarget->target.has_value()
            ? session.World().
                  Universe().
                  ObjectForBody(
                      logicalTarget->
                          target->body)
            : std::nullopt;

    const auto bodyMaterial =
        ResolveRuntimeBodyMaterial(
            content_,
            session.World().Objects(),
            compactBodyObject);

    const auto resolvedCompactForView =
        compactBodyObject.has_value()
            ? world_model::
                  ResolveCompactObject(
                      session.World().
                          Objects(),
                      *compactBodyObject)
            : std::nullopt;

    std::optional<
        world_model::ResolvedAccretionFlow>
        resolvedAccretionForView;

    std::optional<
        celestial_compact_objects::
            CompactObjectPresentation>
        compactPresentation;

    if (resolvedCompactForView.has_value())
    {
        resolvedAccretionForView =
            world_model::
                ResolveAccretionFlow(
                    session.World().
                        Objects(),
                    *compactBodyObject,
                    resolvedCompactForView->
                        parameters);

        compactPresentation =
            celestial_compact_objects::
                BuildCompactObjectPresentation(
                    resolvedCompactForView->
                        parameters);

        const auto accretionProduct =
            resolvedAccretionForView.has_value()
                ? std::optional(
                      celestial_compact_objects::
                          BuildAccretionFlowProduct(
                              resolvedAccretionForView->
                                  parameters,
                              resolvedCompactForView->
                                  parameters,
                              64U))
                : std::nullopt;

        const f64 opticalRadiusMeters =
            std::max(
                compactPresentation->
                    shadowRadiusMeters,
                accretionProduct.has_value()
                    ? accretionProduct->
                          outerRadiusMeters
                    : 0.0);

        const f64 cameraDistance =
            std::max(
                math::Length(
                    camera.
                        localPositionMeters),
                opticalRadiusMeters);

        celestial_representation::ResolveInput
            compactResolveInput{
                .bodyRadiusMeters =
                    std::max(
                        opticalRadiusMeters,
                        1.0),
                .maximumProductionDetailMeters =
                    0.0,
                .maximumMacroDisplacementMeters =
                    0.0,
                .cameraDistanceToCenterMeters =
                    cameraDistance,
                .verticalFieldOfViewRadians =
                    static_cast<f64>(
                        camera.
                            verticalFovRadians),
                .viewportHeightPixels =
                    static_cast<f64>(
                        std::max(
                            height,
                            1U)),
                .features = {
                    .productionSurfaceAvailable =
                        false,
                    .macroDisplacementAvailable =
                        false,
                    .complexFarAppearance =
                        false,
                    .radiativeEmitter =
                        false
                },
                .policy =
                    celestialQualityPolicy_
            };

        const celestial_representation::
            RepresentationSubjectId
            compactSubject{
                .high =
                    logicalTarget->
                        target->body.high,
                .low =
                    logicalTarget->
                        target->body.low
            };

        const auto compactDecision =
            representationTracker_.
                ResolveFor(
                    compactSubject,
                    compactResolveInput);

        const auto compactBlend =
            celestial_representation::
                ResolveRepresentationBlend(
                    compactResolveInput,
                    compactDecision);

        const auto pointWeightFor =
            [](const celestial_representation::
                   Representation representation,
               const f64 weight)
            {
                return representation ==
                           celestial_representation::
                               Representation::
                                   PointProxy ||
                       representation ==
                           celestial_representation::
                               Representation::
                                   StellarPointProxy
                    ? weight
                    : 0.0;
            };

        const f64 pointProxyWeight =
            std::clamp(
                pointWeightFor(
                    compactBlend.richer,
                    compactBlend.richerWeight) +
                pointWeightFor(
                    compactBlend.lower,
                    compactBlend.lowerWeight),
                0.0,
                1.0);

        const f64 projectedOpticalRadiusPixels =
            compactDecision.
                projectedRadiusPixels;

        const f64 projectedShadowRadiusPixels =
            projectedOpticalRadiusPixels *
            compactPresentation->
                shadowRadiusMeters /
            std::max(
                opticalRadiusMeters,
                1.0e-9);

        compactObjectDiagnostics_.
            insert_or_assign(
                info.id,
                StudioCompactObjectDiagnostics{
                    .body =
                        logicalTarget->
                            target->body,
                    .fingerprint =
                        resolvedCompactForView->
                            fingerprint,
                    .gravitationalRadiusMeters =
                        compactPresentation->
                            scales.
                            gravitationalRadiusMeters,
                    .schwarzschildRadiusMeters =
                        compactPresentation->
                            scales.
                            schwarzschildRadiusMeters,
                    .photonSphereRadiusMeters =
                        compactPresentation->
                            scales.
                            photonSphereRadiusMeters,
                    .iscoRadiusMeters =
                        compactPresentation->
                            scales.
                            iscoRadiusMeters,
                    .shadowRadiusMeters =
                        compactPresentation->
                            shadowRadiusMeters,
                    .projectedShadowRadiusPixels =
                        projectedShadowRadiusPixels,
                    .projectedOpticalRadiusPixels =
                        projectedOpticalRadiusPixels,
                    .representation =
                        compactDecision.
                            representation,
                    .pointProxyWeight =
                        pointProxyWeight,
                    .accretionEnabled =
                        resolvedAccretionForView.
                            has_value(),
                    .accretionOuterRadiusMeters =
                        accretionProduct.has_value()
                            ? accretionProduct->
                                  outerRadiusMeters
                            : 0.0
                });

        const auto compactDraw =
            celestial_compact_render::
                CompactObjectDraw{
                    .compact =
                        *compactPresentation,
                    .accretion =
                        resolvedAccretionForView.
                            has_value()
                            ? std::optional(
                                  resolvedAccretionForView->
                                      parameters)
                            : std::nullopt,
                    .camera = camera,
                    .projectedShadowRadiusPixels =
                        projectedShadowRadiusPixels,
                    .projectedOpticalRadiusPixels =
                        projectedOpticalRadiusPixels,
                    .pointProxyWeight =
                        static_cast<f32>(
                            pointProxyWeight),
                    .opacity = 1.0F
                };

        graph.AddPass(
            prefix +
                ".CompactObject",
            {
                {
                    .texture =
                        targets.color,
                    .state =
                        rhi::ResourceState::
                            RenderTarget,
                    .access =
                        render_graph::Access::
                            Write
                },
                {
                    .texture =
                        targets.
                            surfaceBaseRoughness,
                    .state =
                        rhi::ResourceState::
                            RenderTarget,
                    .access =
                        render_graph::Access::
                            Write
                },
                {
                    .texture =
                        targets.
                            surfaceNormalMetallic,
                    .state =
                        rhi::ResourceState::
                            RenderTarget,
                    .access =
                        render_graph::Access::
                            Write
                },
                {
                    .texture =
                        targets.
                            surfaceEmissionClass,
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
             bodySurfaceBaseRoughness,
             bodySurfaceNormalMetallic,
             bodySurfaceEmissionClass,
             width,
             height,
             drawBackgroundBodies,
             compactDraw](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                commands.ClearColorTarget(
                    *color,
                    {
                        .red = 0.0F,
                        .green = 0.0F,
                        .blue = 0.0F,
                        .alpha = 1.0F
                    });
                commands.ClearColorTarget(
                    *bodySurfaceBaseRoughness,
                    {0.0F, 0.0F, 0.0F, 1.0F});
                commands.ClearColorTarget(
                    *bodySurfaceNormalMetallic,
                    {0.0F, 1.0F, 0.0F, 0.0F});
                commands.ClearColorTarget(
                    *bodySurfaceEmissionClass,
                    {0.0F, 0.0F, 0.0F, 0.0F});

                drawBackgroundBodies(commands);

                compactObjectRenderer_.Draw(
                    commands,
                    *color,
                    width,
                    height,
                    compactDraw);
            });

        transitionDiagnostics_.erase(info.id);
    clipmapPlanStats_.erase(info.id);

        return;
    }

    if (perspective &&
        logicalTarget->target.has_value())
    {
        bool radiativeEmitter = false;
        f32 resolvedRadiometricIntensity = 1.0F;
        f32 pointRadiometricIntensity = 1.0F;
        std::optional<
            world_model::ResolvedRadiativeBody>
            resolvedRadiativeForView;

        const auto bodyObject =
            session.World().
                Universe().
                ObjectForBody(
                    logicalTarget->
                        target->body);

        std::optional<
            world_model::ResolvedGiantAppearance>
            resolvedGiantForView;
        std::optional<
            world_model::ResolvedSmallBodyAppearance>
            resolvedSmallBodyForView;

        if (bodyObject.has_value())
        {
            resolvedRadiativeForView =
                world_model::
                    ResolveRadiativeBody(
                        session.World().
                            Objects(),
                        *bodyObject);

            resolvedGiantForView =
                world_model::
                    ResolveGiantAppearance(
                        session.World().
                            Objects(),
                        *bodyObject);

            resolvedSmallBodyForView =
                world_model::
                    ResolveSmallBodyAppearance(
                        session.World().
                            Objects(),
                        *bodyObject);

            if (resolvedGiantForView.has_value() &&
                resolvedSmallBodyForView.has_value())
            {
                throw std::runtime_error(
                    "A body cannot enable Giant Appearance and Small Body Appearance simultaneously.");
            }

            if (resolvedRadiativeForView.has_value())
            {
                const auto& radiative =
                    *resolvedRadiativeForView;
                radiativeEmitter = true;

                const f64 distanceMeters =
                    std::max(
                        math::Length(
                            camera.
                                localPositionMeters),
                        1.0);

                const f64 pointIrradiance =
                    celestial_radiometry::
                        IrradianceWattsPerSquareMeter(
                            radiative.
                                radiative.
                                luminosityWatts,
                            distanceMeters);

                pointRadiometricIntensity =
                    static_cast<f32>(
                        celestial_radiometry::
                            EncodeIrradianceSceneLinear(
                                pointIrradiance));

                // A resolved stellar disc is an extended source whose
                // pixels carry its surface radiance (radiance is
                // invariant with distance). Encode it in the same
                // scene units as the shared direct-lighting pass,
                // where scene value = radiance / reference irradiance
                // (a white Lambertian surface at 1 AU reads 1/pi).
                // The previous per-pixel solid-angle irradiance left
                // a resolved sun orders of magnitude too dark.
                resolvedRadiometricIntensity =
                    static_cast<f32>(
                        radiative.
                            radiative.
                            surfaceRadianceWattsPerSquareMeterSteradian /
                        kStudioReferenceIrradianceWattsPerSquareMeter);
            }
        }

        const f64 radius =
            ReferenceRadiusForShape(
                bodyShape);

        celestial_representation::ResolveInput
            input{
                .bodyRadiusMeters =
                    radius,
                .maximumProductionDetailMeters =
                    0.0,
                .maximumMacroDisplacementMeters =
                    0.0,
                .cameraDistanceToCenterMeters =
                    std::max(
                        math::Length(
                            camera.
                                localPositionMeters),
                        radius),
                .verticalFieldOfViewRadians =
                    static_cast<f64>(
                        camera.
                            verticalFovRadians),
                .viewportHeightPixels =
                    static_cast<f64>(
                        std::max(
                            height,
                            1U)),
                .features = {
                    .productionSurfaceAvailable =
                        false,
                    .macroDisplacementAvailable =
                        false,
                    .complexFarAppearance =
                        false,
                    .radiativeEmitter =
                        radiativeEmitter
                },
                .policy =
                    celestialQualityPolicy_
            };

        const celestial_representation::
            RepresentationSubjectId
            subject{
                .high =
                    logicalTarget->
                        target->body.high,
                .low =
                    logicalTarget->
                        target->body.low
            };

        const auto decision =
            representationTracker_.
                ResolveFor(
                    subject,
                    input);

        const auto blend =
            celestial_representation::
                ResolveRepresentationBlend(
                    input,
                    decision);

        transitionDiagnostics_.
            insert_or_assign(
                info.id,
                StudioSurfaceGlobeTransitionDiagnostics{
                    .body =
                        logicalTarget->
                            target->body,
                    .representation =
                        decision.
                            representation,
                    .lowerFidelityNeighbor =
                        decision.
                            lowerFidelityNeighbor,
                    .richerWeight =
                        blend.richerWeight,
                    .lowerWeight =
                        blend.lowerWeight,
                    .productionSurfaceWeight =
                        0.0,
                    .macroGlobeWeight =
                        0.0,
                    .projectedRadiusPixels =
                        decision.
                            projectedRadiusPixels,
                    .productionDetailErrorPixels =
                        0.0,
                    .macroDisplacementErrorPixels =
                        0.0,
                    .hysteresisHeld =
                        decision.
                            hysteresisHeld,
                    .overlapping =
                        blend.overlapping
                });

        const math::Float3 stellarColor =
            resolvedRadiativeForView.has_value()
                ? math::Float3{
                      static_cast<f32>(
                          resolvedRadiativeForView->
                              stellarColorLinear.x),
                      static_cast<f32>(
                          resolvedRadiativeForView->
                              stellarColorLinear.y),
                      static_cast<f32>(
                          resolvedRadiativeForView->
                              stellarColorLinear.z)}
                : math::Float3{
                      1.0F, 1.0F, 1.0F};

        std::optional<
            celestial_far_render::AppearanceSummary>
            giantAppearanceSummary;
        std::optional<
            celestial_far_render::AppearanceSummary>
            smallBodyAppearanceSummary;
        f64 smallBodyMinimumRadiusScale = 1.0;
        f64 smallBodyMaximumRadiusScale = 1.0;

        if (resolvedGiantForView.has_value())
        {
            auto& giantPresentation =
                giantPresentations_[info.id];

            const bool giantNeedsBuild =
                giantPresentation.appearance ==
                    nullptr ||
                giantPresentation.body !=
                    logicalTarget->
                        target->body ||
                giantPresentation.fingerprint !=
                    resolvedGiantForView->
                        fingerprint;

            if (giantNeedsBuild &&
                acquireCelestialGrant(
                    info.id,
                    logicalTarget->
                        target->body,
                    celestial_scheduler::
                        WorkKind::
                            OrbitalAppearance,
                    resolvedGiantForView->
                        fingerprint,
                    celestial_scheduler::
                        WorkBackend::Cpu,
                    3U,
                    80,
                    true))
            {
                auto giantAppearance =
                    celestial_giants::
                        BuildGiantAppearance(
                            resolvedGiantForView->
                                parameters,
                            {
                                .faceResolution =
                                    65U
                            });

                giantPresentation.summary =
                    celestial_far_render::
                        SummarizeAppearance(
                            giantAppearance);
                giantPresentation.gpuAppearance =
                    std::make_unique<
                        celestial_appearance::
                            GpuPlanetaryAppearanceProduct>(
                                *device_,
                                giantAppearance);
                giantPresentation.appearance =
                    std::make_unique<
                        celestial_appearance::
                            PlanetaryAppearanceProduct>(
                                std::move(
                                    giantAppearance));
                giantPresentation.body =
                    logicalTarget->
                        target->body;
                giantPresentation.fingerprint =
                    resolvedGiantForView->
                        fingerprint;

                static_cast<void>(
                    completeCelestialGrant(
                        info.id,
                        logicalTarget->
                            target->body,
                        celestial_scheduler::
                            WorkKind::
                                OrbitalAppearance,
                        resolvedGiantForView->
                            fingerprint));
            }

            if (giantPresentation.appearance !=
                    nullptr &&
                giantPresentation.body ==
                    logicalTarget->
                        target->body &&
                giantPresentation.fingerprint ==
                    resolvedGiantForView->
                        fingerprint)
            {
                giantAppearanceSummary =
                    giantPresentation.summary;
            }
        }
        else
        {
            giantPresentations_.erase(
                info.id);
            giantDiagnostics_.erase(
                info.id);
        }

        if (resolvedSmallBodyForView.has_value())
        {
            auto& smallPresentation =
                smallBodyPresentations_[info.id];

            const bool smallBodyNeedsBuild =
                smallPresentation.appearance ==
                    nullptr ||
                smallPresentation.body !=
                    logicalTarget->
                        target->body ||
                smallPresentation.fingerprint !=
                    resolvedSmallBodyForView->
                        fingerprint;

            if (smallBodyNeedsBuild &&
                acquireCelestialGrant(
                    info.id,
                    logicalTarget->
                        target->body,
                    celestial_scheduler::
                        WorkKind::
                            OrbitalAppearance,
                    resolvedSmallBodyForView->
                        fingerprint,
                    celestial_scheduler::
                        WorkBackend::Cpu,
                    3U,
                    80,
                    true))
            {
                auto smallAppearance =
                    celestial_small_bodies::
                        BuildSmallBodyAppearance(
                            resolvedSmallBodyForView->
                                parameters,
                            {
                                .faceResolution =
                                    65U
                            });

                const auto smallShape =
                    celestial_small_bodies::
                        BuildSmallBodyShape(
                            resolvedSmallBodyForView->
                                parameters,
                            {
                                .faceResolution =
                                    65U
                            });

                smallPresentation.summary =
                    celestial_far_render::
                        SummarizeAppearance(
                            smallAppearance);
                smallPresentation.gpuAppearance =
                    std::make_unique<
                        celestial_appearance::
                            GpuPlanetaryAppearanceProduct>(
                                *device_,
                                smallAppearance);
                smallPresentation.appearance =
                    std::make_unique<
                        celestial_appearance::
                            PlanetaryAppearanceProduct>(
                                std::move(
                                    smallAppearance));
                smallPresentation.body =
                    logicalTarget->
                        target->body;
                smallPresentation.fingerprint =
                    resolvedSmallBodyForView->
                        fingerprint;
                smallPresentation.minimumRadiusScale =
                    smallShape.minimumRadiusScale;
                smallPresentation.maximumRadiusScale =
                    smallShape.maximumRadiusScale;

                static_cast<void>(
                    completeCelestialGrant(
                        info.id,
                        logicalTarget->
                            target->body,
                        celestial_scheduler::
                            WorkKind::
                                OrbitalAppearance,
                        resolvedSmallBodyForView->
                            fingerprint));
            }

            if (smallPresentation.appearance !=
                    nullptr &&
                smallPresentation.body ==
                    logicalTarget->
                        target->body &&
                smallPresentation.fingerprint ==
                    resolvedSmallBodyForView->
                        fingerprint)
            {
                smallBodyAppearanceSummary =
                    smallPresentation.summary;
                smallBodyMinimumRadiusScale =
                    smallPresentation.
                        minimumRadiusScale;
                smallBodyMaximumRadiusScale =
                    smallPresentation.
                        maximumRadiusScale;
            }
        }
        else
        {
            smallBodyPresentations_.erase(
                info.id);
            smallBodyDiagnostics_.erase(
                info.id);
        }

        celestial_far_render::
            AppearanceSummary appearance{
                .albedoLinear =
                    radiativeEmitter
                        ? stellarColor
                        : giantAppearanceSummary.has_value()
                            ? giantAppearanceSummary->
                                  albedoLinear
                            : smallBodyAppearanceSummary.has_value()
                                ? smallBodyAppearanceSummary->
                                      albedoLinear
                                : math::Float3{
                                      0.18F,
                                      0.21F,
                                      0.23F},
                .roughness =
                    radiativeEmitter
                        ? 0.0F
                        : giantAppearanceSummary.has_value()
                            ? giantAppearanceSummary->
                                  roughness
                            : smallBodyAppearanceSummary.has_value()
                                ? smallBodyAppearanceSummary->
                                      roughness
                                : 0.82F,
                .oceanFraction = 0.0F,
                .iceFraction = 0.0F,
                .emissionLinear = {}
            };

        const auto richer =
            blend.richer;
        const auto lower =
            blend.lower;
        const f32 lowerOpacity =
            static_cast<f32>(
                std::clamp(
                    blend.lowerWeight,
                    0.0,
                    1.0));
        const f64 projectedRadius =
            decision.
                projectedRadiusPixels;

        if (resolvedRadiativeForView.has_value())
        {
            stellarDiagnostics_.insert_or_assign(
                info.id,
                StudioStellarDiagnostics{
                    .body =
                        logicalTarget->
                            target->body,
                    .appearanceFingerprint =
                        resolvedRadiativeForView->
                            stellarAppearanceFingerprint,
                    .effectiveTemperatureKelvin =
                        resolvedRadiativeForView->
                            radiative.
                            effectiveTemperatureKelvin,
                    .colorLinear =
                        stellarColor,
                    .projectedRadiusPixels =
                        projectedRadius,
                    .representation =
                        decision.representation,
                    .resolvedSceneIntensity =
                        resolvedRadiometricIntensity,
                    .pointSceneIntensity =
                        pointRadiometricIntensity
                });
        }
        else
        {
            stellarDiagnostics_.erase(
                info.id);
        }

        if (resolvedGiantForView.has_value())
        {
            giantDiagnostics_.insert_or_assign(
                info.id,
                StudioGiantDiagnostics{
                    .body =
                        logicalTarget->
                            target->body,
                    .appearanceFingerprint =
                        resolvedGiantForView->
                            fingerprint,
                    .iceGiant =
                        resolvedGiantForView->
                            parameters.giantClass ==
                        celestial_giants::
                            GiantClass::IceGiant,
                    .bandFrequency =
                        resolvedGiantForView->
                            parameters.bandFrequency,
                    .bandStrength =
                        resolvedGiantForView->
                            parameters.bandStrength,
                    .stormStrength =
                        resolvedGiantForView->
                            parameters.stormStrength,
                    .projectedRadiusPixels =
                        projectedRadius,
                    .representation =
                        decision.representation
                });
        }

        if (resolvedSmallBodyForView.has_value())
        {
            smallBodyDiagnostics_.insert_or_assign(
                info.id,
                StudioSmallBodyDiagnostics{
                    .body =
                        logicalTarget->
                            target->body,
                    .appearanceFingerprint =
                        resolvedSmallBodyForView->
                            fingerprint,
                    .minimumRadiusScale =
                        smallBodyMinimumRadiusScale,
                    .maximumRadiusScale =
                        smallBodyMaximumRadiusScale,
                    .irregularity =
                        resolvedSmallBodyForView->
                            parameters.irregularity,
                    .craterDensity =
                        resolvedSmallBodyForView->
                            parameters.craterDensity,
                    .oppositionStrength =
                        resolvedSmallBodyForView->
                            parameters.oppositionStrength,
                    .projectedRadiusPixels =
                        projectedRadius,
                    .representation =
                        decision.representation
                });
        }

        graph.AddPass(
            prefix +
                ".FarBody",
            {
                {
                    .texture = targets.color,
                    .state =
                        rhi::ResourceState::
                            RenderTarget,
                    .access =
                        render_graph::Access::
                            Write
                },
                {
                    .texture = targets.surfaceBaseRoughness,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                },
                {
                    .texture = targets.surfaceNormalMetallic,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                },
                {
                    .texture = targets.surfaceEmissionClass,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                }
            },
            [this,
             color,
             bodySurfaceBaseRoughness,
             bodySurfaceNormalMetallic,
             bodySurfaceEmissionClass,
             width,
             height,
             drawBackgroundBodies,
             bodyShape,
             camera,
             appearance,
             richer,
             lower,
             lowerOpacity,
             projectedRadius,
             radiativeEmitter,
             resolvedRadiometricIntensity,
             pointRadiometricIntensity,
             resolvedRadiativeForView,
             resolvedGiantForView,
             resolvedSmallBodyForView,
             studioDirectLight,
             resolvedOceanForView](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                commands.ClearColorTarget(
                    *color,
                    {
                        .red = 0.0F,
                        .green = 0.0F,
                        .blue = 0.0F,
                        .alpha = 1.0F
                    });
                commands.ClearColorTarget(
                    *bodySurfaceBaseRoughness,
                    {0.0F, 0.0F, 0.0F, 1.0F});
                commands.ClearColorTarget(
                    *bodySurfaceNormalMetallic,
                    {0.0F, 1.0F, 0.0F, 0.0F});
                commands.ClearColorTarget(
                    *bodySurfaceEmissionClass,
                    {0.0F, 0.0F, 0.0F, 0.0F});

                drawBackgroundBodies(commands);

                const auto draw =
                    [&](const celestial_representation::
                            Representation representation,
                        const f32 opacity)
                    {
                        celestial_far_render::
                            FarBodyDraw far{
                                .representation =
                                    representation,
                                .shape =
                                    bodyShape,
                                .camera =
                                    camera,
                                .appearance =
                                    appearance,
                                .projectedRadiusPixels =
                                    projectedRadius,
                                .opacity =
                                    opacity,
                                .radiometricIntensity =
                                    radiativeEmitter
                                        ? (representation ==
                                                   celestial_representation::
                                                       Representation::
                                                           PointProxy ||
                                           representation ==
                                               celestial_representation::
                                                   Representation::
                                                       StellarPointProxy
                                               ? pointRadiometricIntensity
                                               : resolvedRadiometricIntensity)
                                        : 1.0F,
                                .lightDirectionBody =
                                    studioDirectLight.
                                        directionBody,
                                .incidentLightScale =
                                    radiativeEmitter
                                        ? 1.0F
                                        : studioDirectLight.
                                            irradianceScale,
                                .oceanRefractiveIndex =
                                    static_cast<f32>(
                                        resolvedOceanForView.has_value()
                                            ? resolvedOceanForView->optical.refractiveIndex
                                            : 1.333),
                                .oceanRoughness =
                                    static_cast<f32>(
                                        resolvedOceanForView.has_value()
                                            ? resolvedOceanForView->optical.orbitalRoughness
                                            : 0.12),
                                .oceanGlintStrength =
                                    static_cast<f32>(
                                        resolvedOceanForView.has_value()
                                            ? resolvedOceanForView->optical.glintStrength
                                            : 1.0),
                                .oceanEnabled =
                                    resolvedOceanForView.has_value(),
                                .giantEnabled =
                                    resolvedGiantForView.has_value(),
                                .giantBaseColorLinear =
                                    resolvedGiantForView.has_value()
                                        ? math::Float3{
                                              static_cast<f32>(resolvedGiantForView->parameters.baseColorLinear.x),
                                              static_cast<f32>(resolvedGiantForView->parameters.baseColorLinear.y),
                                              static_cast<f32>(resolvedGiantForView->parameters.baseColorLinear.z)}
                                        : math::Float3{0.62F,0.48F,0.31F},
                                .giantBandColorLinear =
                                    resolvedGiantForView.has_value()
                                        ? math::Float3{
                                              static_cast<f32>(resolvedGiantForView->parameters.bandColorLinear.x),
                                              static_cast<f32>(resolvedGiantForView->parameters.bandColorLinear.y),
                                              static_cast<f32>(resolvedGiantForView->parameters.bandColorLinear.z)}
                                        : math::Float3{0.90F,0.78F,0.58F},
                                .giantPolarColorLinear =
                                    resolvedGiantForView.has_value()
                                        ? math::Float3{
                                              static_cast<f32>(resolvedGiantForView->parameters.polarColorLinear.x),
                                              static_cast<f32>(resolvedGiantForView->parameters.polarColorLinear.y),
                                              static_cast<f32>(resolvedGiantForView->parameters.polarColorLinear.z)}
                                        : math::Float3{0.48F,0.42F,0.36F},
                                .giantBandFrequency =
                                    static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.bandFrequency:11.0),
                                .giantBandStrength =
                                    static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.bandStrength:0.72),
                                .giantZonalShear =
                                    static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.zonalShear:0.18),
                                .giantStormStrength =
                                    static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.stormStrength:0.35),
                                .giantStormScale =
                                    static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.stormScale:5.0),
                                .giantPolarStrength =
                                    static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.polarStrength:0.22),
                                .giantDepthContrast =
                                    static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.depthContrast:0.25),
                                .giantTurbulenceStrength =
                                    static_cast<f32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.turbulenceStrength:0.18),
                                .giantSeed =
                                    static_cast<u32>(resolvedGiantForView.has_value()?resolvedGiantForView->parameters.seed&0xffffffffULL:1ULL),
                                .smallBodyEnabled =
                                    resolvedSmallBodyForView.has_value(),
                                .smallBodyAxisScale =
                                    resolvedSmallBodyForView.has_value()
                                        ? math::Float3{
                                              static_cast<f32>(resolvedSmallBodyForView->parameters.axisScale.x),
                                              static_cast<f32>(resolvedSmallBodyForView->parameters.axisScale.y),
                                              static_cast<f32>(resolvedSmallBodyForView->parameters.axisScale.z)}
                                        : math::Float3{1.0F,0.82F,0.68F},
                                .smallBodyIrregularity =
                                    static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.irregularity:0.18),
                                .smallBodyLargeLobeStrength =
                                    static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.largeLobeStrength:0.12),
                                .smallBodyCraterDensity =
                                    static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.craterDensity:0.55),
                                .smallBodyCraterDepth =
                                    static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.craterDepth:0.12),
                                .smallBodyCraterRimStrength =
                                    static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.craterRimStrength:0.08),
                                .smallBodyFreshMaterialColorLinear =
                                    resolvedSmallBodyForView.has_value()
                                        ? math::Float3{
                                              static_cast<f32>(resolvedSmallBodyForView->parameters.freshMaterialColorLinear.x),
                                              static_cast<f32>(resolvedSmallBodyForView->parameters.freshMaterialColorLinear.y),
                                              static_cast<f32>(resolvedSmallBodyForView->parameters.freshMaterialColorLinear.z)}
                                        : math::Float3{0.24F,0.22F,0.19F},
                                .smallBodyColorVariation =
                                    static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.colorVariation:0.18),
                                .smallBodyOppositionStrength =
                                    static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.oppositionStrength:0.55),
                                .smallBodyOppositionWidthRadians =
                                    static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.oppositionWidthRadians:0.055),
                                .smallBodySingleScatteringAlbedo =
                                    static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.singleScatteringAlbedo:0.16),
                                .smallBodyMacroscopicRoughnessRadians =
                                    static_cast<f32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.macroscopicRoughnessRadians:0.42),
                                .smallBodySeed =
                                    static_cast<u32>(resolvedSmallBodyForView.has_value()?resolvedSmallBodyForView->parameters.seed&0xffffffffULL:1ULL),
                                .stellar =
                                    radiativeEmitter,
                                .stellarColorLinear =
                                    resolvedRadiativeForView.has_value()
                                        ? math::Float3{
                                              static_cast<f32>(
                                                  resolvedRadiativeForView->
                                                      stellarColorLinear.x),
                                              static_cast<f32>(
                                                  resolvedRadiativeForView->
                                                      stellarColorLinear.y),
                                              static_cast<f32>(
                                                  resolvedRadiativeForView->
                                                      stellarColorLinear.z)}
                                        : math::Float3{
                                              1.0F, 1.0F, 1.0F},
                                .stellarLimbDarkening =
                                    static_cast<f32>(
                                        resolvedRadiativeForView.has_value()
                                            ? resolvedRadiativeForView->
                                                  stellarAppearance.
                                                  limbDarkening
                                            : 0.58),
                                .stellarGranulationStrength =
                                    static_cast<f32>(
                                        resolvedRadiativeForView.has_value()
                                            ? resolvedRadiativeForView->
                                                  stellarAppearance.
                                                  granulationStrength
                                            : 0.10),
                                .stellarGranulationScale =
                                    static_cast<f32>(
                                        resolvedRadiativeForView.has_value()
                                            ? resolvedRadiativeForView->
                                                  stellarAppearance.
                                                  granulationScale
                                            : 42.0),
                                .stellarActivityLevel =
                                    static_cast<f32>(
                                        resolvedRadiativeForView.has_value()
                                            ? resolvedRadiativeForView->
                                                  stellarAppearance.
                                                  activityLevel
                                            : 0.12),
                                .stellarActivitySeed =
                                    static_cast<u32>(
                                        resolvedRadiativeForView.has_value()
                                            ? resolvedRadiativeForView->
                                                  stellarAppearance.
                                                  activitySeed &
                                                  0xffffffffULL
                                            : 1ULL),
                                .stellarChromosphereStrength =
                                    static_cast<f32>(
                                        resolvedRadiativeForView.has_value()
                                            ? resolvedRadiativeForView->
                                                  stellarAppearance.
                                                  chromosphereStrength
                                            : 0.08),
                                .stellarChromosphereExtent =
                                    static_cast<f32>(
                                        resolvedRadiativeForView.has_value()
                                            ? resolvedRadiativeForView->
                                                  stellarAppearance.
                                                  chromosphereExtent
                                            : 0.035),
                                .stellarCoronaStrength =
                                    static_cast<f32>(
                                        resolvedRadiativeForView.has_value()
                                            ? resolvedRadiativeForView->
                                                  stellarAppearance.
                                                  coronaStrength
                                            : 0.025),
                                .stellarCoronaExtent =
                                    static_cast<f32>(
                                        resolvedRadiativeForView.has_value()
                                            ? resolvedRadiativeForView->
                                                  stellarAppearance.
                                                  coronaExtent
                                            : 1.75),
                                .stellarGlareStrength =
                                    static_cast<f32>(
                                        resolvedRadiativeForView.has_value()
                                            ? resolvedRadiativeForView->
                                                  stellarAppearance.
                                                  glareStrength
                                            : 0.35),
                                .stellarGlareRadiusPixels =
                                    static_cast<f32>(
                                        resolvedRadiativeForView.has_value()
                                            ? resolvedRadiativeForView->
                                                  stellarAppearance.
                                                  glareRadiusPixels
                                            : 5.0)
                            };

                        farBodyRenderer_.Draw(
                            commands,
                            *color,
                            width,
                            height,
                            far,
                            nullptr);
                        if (opacity >= 0.5F)
                        {
                            farBodyRenderer_.DrawSurfaceData(
                                commands,
                                *bodySurfaceBaseRoughness,
                                *bodySurfaceNormalMetallic,
                                *bodySurfaceEmissionClass,
                                width,
                                height,
                                far);
                        }
                    };

                draw(
                    richer,
                    1.0F);

                if (lower != richer)
                {
                    draw(
                        lower,
                        lowerOpacity);
                }
            });
    }
    else
    {
        transitionDiagnostics_.erase(info.id);
    clipmapPlanStats_.erase(info.id);

        graph.AddPass(
            prefix + ".Body",
            {
                {
                    .texture = targets.color,
                    .state =
                        rhi::ResourceState::
                            RenderTarget,
                    .access =
                        render_graph::Access::
                            Write
                },
                {
                    .texture = targets.surfaceBaseRoughness,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                },
                {
                    .texture = targets.surfaceNormalMetallic,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                },
                {
                    .texture = targets.surfaceEmissionClass,
                    .state = rhi::ResourceState::RenderTarget,
                    .access = render_graph::Access::Write
                }
            },
            [this,
             color,
             bodySurfaceBaseRoughness,
             bodySurfaceNormalMetallic,
             bodySurfaceEmissionClass,
             width,
             height,
             drawBackgroundBodies,
             camera,
             bodyShape,
             bodyMaterial](
                rhi::CommandList& commands,
                const render_graph::Resources&)
            {
                commands.ClearColorTarget(
                    *color,
                    {0.0F, 0.0F, 0.0F, 1.0F});
                commands.ClearColorTarget(
                    *bodySurfaceBaseRoughness,
                    {0.0F, 0.0F, 0.0F, 1.0F});
                commands.ClearColorTarget(
                    *bodySurfaceNormalMetallic,
                    {0.0F, 1.0F, 0.0F, 0.0F});
                commands.ClearColorTarget(
                    *bodySurfaceEmissionClass,
                    {0.0F, 0.0F, 0.0F, 0.0F});

                drawBackgroundBodies(commands);

                bodyRenderer_.Draw(
                    commands,
                    *color,
                    width,
                    height,
                    bodyShape,
                    camera,
                    bodyMaterial,
                    false);
                bodyRenderer_.DrawSurfaceData(
                    commands,
                    *bodySurfaceBaseRoughness,
                    *bodySurfaceNormalMetallic,
                    *bodySurfaceEmissionClass,
                    width,
                    height,
                    bodyShape,
                    camera,
                    bodyMaterial);
            });
    }
}
} // namespace orbit::studio_ui
