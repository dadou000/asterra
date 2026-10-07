#include "StudioViewportInternals.hpp"

namespace orbit::studio_ui::viewport_detail
{

[[nodiscard]] math::Double3x3 EulerDegreesToRotation(
    const math::Double3& eulerDegrees) noexcept
{
    constexpr f64 kDegreesToRadians =
        0.017453292519943295769;

    const f64 rx =
        eulerDegrees.x * kDegreesToRadians;
    const f64 ry =
        eulerDegrees.y * kDegreesToRadians;
    const f64 rz =
        eulerDegrees.z * kDegreesToRadians;

    const f64 cx = std::cos(rx);
    const f64 sx = std::sin(rx);
    const f64 cy = std::cos(ry);
    const f64 sy = std::sin(ry);
    const f64 cz = std::cos(rz);
    const f64 sz = std::sin(rz);

    // Intrinsic XYZ (equivalent parent-space Rz * Ry * Rx).
    return {
        .xAxis = {
            cz * cy,
            sz * cy,
            -sy
        },
        .yAxis = {
            cz * sy * sx - sz * cx,
            sz * sy * sx + cz * cx,
            cy * sx
        },
        .zAxis = {
            cz * sy * cx + sz * sx,
            sz * sy * cx - cz * sx,
            cy * cx
        }
    };
}

[[nodiscard]] std::vector<lighting::VisibilityProxy>
BuildVisibilityProxies(
    const std::vector<
        world_model::ResolvedVisibilityProxy>& resolved,
    const universe::BodyId body,
    const frames::FrameId bodyFrame)
{
    std::vector<lighting::VisibilityProxy> proxies;
    proxies.reserve(resolved.size());

    for (const auto& source : resolved)
    {
        proxies.push_back({
            .stableId =
                source.object.high ^
                source.object.low,
            .body = body,
            .frame = bodyFrame,
            .frameFromProxy = {
                .rotation =
                    EulerDegreesToRotation(
                        source.eulerDegrees),
                .translation =
                    source.positionMeters
            },
            .shape =
                source.shape ==
                        world_model::
                            ResolvedVisibilityProxyShape::
                                Box
                    ? lighting::
                        VisibilityProxyShape::Box
                    : lighting::
                        VisibilityProxyShape::Sphere,
            .sphereRadiusMeters =
                source.radiusMeters,
            .boxHalfExtentsMeters =
                source.halfExtentsMeters,
            .materialId =
                source.materialId,
            .instanceId =
                source.instanceId,
            .nominalErrorMeters =
                source.
                    maximumApproximationErrorMeters,
            .dynamic =
                source.dynamic
        });
    }

    return proxies;
}

[[nodiscard]] f64 ReferenceRadiusForShape(
    const universe::BodyShape& shape)
{
    return std::visit(
        [](const auto& value) -> f64
        {
            using Shape =
                std::decay_t<
                    decltype(value)>;

            if constexpr (
                std::is_same_v<
                    Shape,
                    universe::SphereShape>)
            {
                return value.radiusMeters;
            }
            else
            {
                return std::max({
                    value.radiiMeters.x,
                    value.radiiMeters.y,
                    value.radiiMeters.z
                });
            }
        },
        shape);
}

[[nodiscard]] ResolvedStudioDirectLight
ResolveStudioDirectLight(
    studio_session::StudioSession& session,
    const universe::BodyId receiver,
    const time::SimulationTime atTime)
{
    ResolvedStudioDirectLight result;
    world_model::CelestialLightingService
        lighting(
            session.World().Objects(),
            session.World().Universe());
    result.direct = lighting.DominantDirectLightingAtBody(receiver, atTime);
    if (result.direct.has_value())
    {
        const auto direction = math::Normalize(
            result.direct->receiverBodyFixedToEmitterMeters);
        result.directionBody = {
            static_cast<f32>(direction.x),
            static_cast<f32>(direction.y),
            static_cast<f32>(direction.z)
        };
        result.irradianceScale = static_cast<f32>(
            result.direct->irradianceWattsPerSquareMeter /
            kStudioReferenceIrradianceWattsPerSquareMeter);
    }

    return result;
}

[[nodiscard]] std::vector<celestial_far_render::FarBodyDraw>
ResolveSystemBodyDraws(
    studio_session::StudioSession& session,
    content::ContentService* content,
    const universe::BodyId activeBody,
    const render_view::CameraState& camera,
    const time::SimulationTime atTime,
    const u32 width,
    const u32 height)
{
    struct DistanceDraw
    {
        f64 distanceMeters{0.0};
        celestial_far_render::FarBodyDraw draw;
    };
    std::vector<DistanceDraw> sorted;
    const auto& universe = session.World().Universe();
    const auto* active = universe.Bodies().FindBody(activeBody);
    if (active == nullptr || !camera.frame)
    {
        return {};
    }
    const auto& frames = universe.Frames();
    const auto& objects = session.World().Objects();
    const world_model::CelestialLightingService lighting(objects, universe);
    for (const auto bodyId : universe.Bodies().Bodies(active->system))
    {
        if (bodyId == activeBody)
        {
            continue;
        }
        const auto* body = universe.Bodies().FindBody(bodyId);
        if (body == nullptr)
        {
            continue;
        }
        const auto center = frames.TransformPoint(
            {.frame = body->frame, .localMeters = {}},
            camera.frame, atTime);
        const auto bodyFromCamera = frames.ResolveTransform(
            camera.frame, body->frame, atTime);
        if (!center.has_value() || !bodyFromCamera.has_value())
        {
            continue;
        }
        const auto projected = render_view::ProjectToViewport(
            camera, width, height, center->localMeters);
        if (!projected.has_value())
        {
            continue;
        }
        const f64 distance = math::Length(
            center->localMeters - camera.localPositionMeters);
        const auto bodyObject = universe.ObjectForBody(bodyId);
        const auto radiative = bodyObject.has_value()
            ? world_model::ResolveRadiativeBody(objects, *bodyObject)
            : std::nullopt;
        const f64 radius = radiative.has_value()
            ? radiative->photosphereRadiusMeters
            : ReferenceRadiusForShape(body->shape);
        if (!std::isfinite(distance) || !std::isfinite(radius) ||
            radius <= 0.0 || distance <= radius)
        {
            continue;
        }
        const f64 tanHalfFov = std::tan(
            static_cast<f64>(camera.verticalFovRadians) * 0.5);
        const f64 apparentRadius =
            radius / std::sqrt(distance * distance - radius * radius);
        const f64 radiusPixels = apparentRadius /
            std::max(tanHalfFov, 1.0e-6) * static_cast<f64>(height) * 0.5;
        const f64 radiusNdc = 2.0 * radiusPixels / static_cast<f64>(height);
        const f64 aspect = static_cast<f64>(width) / static_cast<f64>(height);
        // ProjectToViewport uses up x forward for screen-right, whereas the
        // Studio sky and far-body shaders use forward x up. Match the rendered
        // view so a distant body stays aligned while the camera yaws.
        const f64 centerX = 1.0 - 2.0 * projected->u;
        const f64 centerY = 1.0 - 2.0 * projected->v;
        if (std::abs(centerX) > 1.0 + radiusNdc / aspect + 0.1 ||
            std::abs(centerY) > 1.0 + radiusNdc + 0.1)
        {
            continue;
        }

        celestial_far_render::FarBodyDraw draw;
        draw.shape = body->shape;
        draw.camera = camera;
        draw.camera.frame = body->frame;
        draw.camera.localPositionMeters = math::TransformPoint(
            *bodyFromCamera, camera.localPositionMeters);
        const auto forward = math::TransformVector(
            bodyFromCamera->rotation,
            {camera.forward.x, camera.forward.y, camera.forward.z});
        const auto up = math::TransformVector(
            bodyFromCamera->rotation,
            {camera.up.x, camera.up.y, camera.up.z});
        draw.camera.forward = {
            static_cast<f32>(forward.x),
            static_cast<f32>(forward.y),
            static_cast<f32>(forward.z)};
        draw.camera.up = {
            static_cast<f32>(up.x),
            static_cast<f32>(up.y),
            static_cast<f32>(up.z)};
        draw.projectedRadiusPixels = radiusPixels;
        draw.screenCenterNdc = {
            static_cast<f32>(centerX), static_cast<f32>(centerY)};

        draw.stellar = radiative.has_value();
        draw.representation = radiusPixels >= 10.0 && !draw.stellar
            ? celestial_representation::Representation::SmoothGlobe
            : radiusPixels >= 0.55
                ? celestial_representation::Representation::AnalyticDiscImpostor
                : draw.stellar
                    ? celestial_representation::Representation::StellarPointProxy
                    : celestial_representation::Representation::PointProxy;
        if (draw.representation !=
            celestial_representation::Representation::SmoothGlobe)
        {
            // Disc normals face the body's actual camera direction even when
            // its centre is far from the middle of the viewport.
            const auto toCenter = math::Normalize(
                draw.camera.localPositionMeters * -1.0);
            draw.camera.forward = {
                static_cast<f32>(toCenter.x),
                static_cast<f32>(toCenter.y),
                static_cast<f32>(toCenter.z)};
        }

        if (radiative.has_value())
        {
            draw.shape = universe::SphereShape{
                radiative->photosphereRadiusMeters};
            draw.appearance.albedoLinear = {
                static_cast<f32>(radiative->stellarColorLinear.x),
                static_cast<f32>(radiative->stellarColorLinear.y),
                static_cast<f32>(radiative->stellarColorLinear.z)};
            draw.stellarColorLinear = draw.appearance.albedoLinear;
            draw.radiometricIntensity = static_cast<f32>(
                draw.representation ==
                    celestial_representation::Representation::StellarPointProxy
                    ? celestial_radiometry::EncodeIrradianceSceneLinear(
                          celestial_radiometry::IrradianceWattsPerSquareMeter(
                              radiative->radiative.luminosityWatts, distance))
                    : radiative->radiative.surfaceRadianceWattsPerSquareMeterSteradian /
                          kStudioReferenceIrradianceWattsPerSquareMeter);
            const auto& appearance = radiative->stellarAppearance;
            draw.stellarLimbDarkening = static_cast<f32>(appearance.limbDarkening);
            draw.stellarGranulationStrength = static_cast<f32>(appearance.granulationStrength);
            draw.stellarGranulationScale = static_cast<f32>(appearance.granulationScale);
            draw.stellarActivityLevel = static_cast<f32>(appearance.activityLevel);
            draw.stellarActivitySeed = static_cast<u32>(appearance.activitySeed);
            draw.stellarChromosphereStrength = static_cast<f32>(appearance.chromosphereStrength);
            draw.stellarChromosphereExtent = static_cast<f32>(appearance.chromosphereExtent);
            draw.stellarCoronaStrength = static_cast<f32>(appearance.coronaStrength);
            draw.stellarCoronaExtent = static_cast<f32>(appearance.coronaExtent);
            draw.stellarGlareStrength = static_cast<f32>(appearance.glareStrength);
            draw.stellarGlareRadiusPixels = static_cast<f32>(appearance.glareRadiusPixels);
        }
        else
        {
            draw.appearance.albedoLinear = {0.18F, 0.21F, 0.23F};
            if (const auto material = ResolveRuntimeBodyMaterialIfAssigned(
                    content, session.World().Objects(), bodyObject))
            {
                draw.appearance.albedoLinear = material->baseColor;
                draw.appearance.roughness = material->roughness;
                draw.appearance.emissionLinear = material->emissionRadiance;
            }
            if (const auto direct = lighting.DominantDirectLightingAtBody(bodyId, atTime))
            {
                const auto direction = math::Normalize(
                    direct->receiverBodyFixedToEmitterMeters);
                draw.lightDirectionBody = {
                    static_cast<f32>(direction.x),
                    static_cast<f32>(direction.y),
                    static_cast<f32>(direction.z)};
                draw.incidentLightScale = static_cast<f32>(
                    direct->irradianceWattsPerSquareMeter /
                    kStudioReferenceIrradianceWattsPerSquareMeter);
            }
            else
            {
                draw.incidentLightScale = 0.0F;
            }
            if (bodyObject.has_value())
            {
                if (const auto giant = world_model::ResolveGiantAppearance(
                        objects, *bodyObject))
                {
                    const auto& p = giant->parameters;
                    draw.giantEnabled = true;
                    draw.giantBaseColorLinear = {
                        static_cast<f32>(p.baseColorLinear.x),
                        static_cast<f32>(p.baseColorLinear.y),
                        static_cast<f32>(p.baseColorLinear.z)};
                    draw.giantBandColorLinear = {
                        static_cast<f32>(p.bandColorLinear.x),
                        static_cast<f32>(p.bandColorLinear.y),
                        static_cast<f32>(p.bandColorLinear.z)};
                    draw.giantPolarColorLinear = {
                        static_cast<f32>(p.polarColorLinear.x),
                        static_cast<f32>(p.polarColorLinear.y),
                        static_cast<f32>(p.polarColorLinear.z)};
                    draw.giantBandFrequency = static_cast<f32>(p.bandFrequency);
                    draw.giantBandStrength = static_cast<f32>(p.bandStrength);
                    draw.giantZonalShear = static_cast<f32>(p.zonalShear);
                    draw.giantStormStrength = static_cast<f32>(p.stormStrength);
                    draw.giantStormScale = static_cast<f32>(p.stormScale);
                    draw.giantPolarStrength = static_cast<f32>(p.polarStrength);
                    draw.giantDepthContrast = static_cast<f32>(p.depthContrast);
                    draw.giantTurbulenceStrength = static_cast<f32>(p.turbulenceStrength);
                    draw.giantSeed = static_cast<u32>(p.seed);
                }
                if (const auto small = world_model::ResolveSmallBodyAppearance(
                        objects, *bodyObject))
                {
                    const auto& p = small->parameters;
                    draw.smallBodyEnabled = true;
                    draw.appearance.albedoLinear = {
                        static_cast<f32>(p.regolithColorLinear.x),
                        static_cast<f32>(p.regolithColorLinear.y),
                        static_cast<f32>(p.regolithColorLinear.z)};
                    draw.smallBodyAxisScale = {
                        static_cast<f32>(p.axisScale.x),
                        static_cast<f32>(p.axisScale.y),
                        static_cast<f32>(p.axisScale.z)};
                    draw.smallBodyIrregularity = static_cast<f32>(p.irregularity);
                    draw.smallBodyLargeLobeStrength = static_cast<f32>(p.largeLobeStrength);
                    draw.smallBodyCraterDensity = static_cast<f32>(p.craterDensity);
                    draw.smallBodyCraterDepth = static_cast<f32>(p.craterDepth);
                    draw.smallBodyCraterRimStrength = static_cast<f32>(p.craterRimStrength);
                    draw.smallBodyFreshMaterialColorLinear = {
                        static_cast<f32>(p.freshMaterialColorLinear.x),
                        static_cast<f32>(p.freshMaterialColorLinear.y),
                        static_cast<f32>(p.freshMaterialColorLinear.z)};
                    draw.smallBodyColorVariation = static_cast<f32>(p.colorVariation);
                    draw.smallBodyOppositionStrength = static_cast<f32>(p.oppositionStrength);
                    draw.smallBodyOppositionWidthRadians = static_cast<f32>(p.oppositionWidthRadians);
                    draw.smallBodySingleScatteringAlbedo = static_cast<f32>(p.singleScatteringAlbedo);
                    draw.smallBodyMacroscopicRoughnessRadians = static_cast<f32>(p.macroscopicRoughnessRadians);
                    draw.smallBodySeed = static_cast<u32>(p.seed);
                }
            }
        }
        sorted.push_back({distance, draw});
    }
    std::stable_sort(sorted.begin(), sorted.end(),
        [](const DistanceDraw& a, const DistanceDraw& b) {
            return a.distanceMeters > b.distanceMeters;
        });
    std::vector<celestial_far_render::FarBodyDraw> draws;
    draws.reserve(sorted.size());
    for (auto& item : sorted)
    {
        draws.push_back(std::move(item.draw));
    }
    return draws;
}

[[nodiscard]] std::vector<lighting::ResolvedLocalLight>
ResolveStudioLocalLights(
    studio_session::StudioSession& session,
    const lighting::LightingView& lightingView,
    const std::optional<scene::ObjectId> root)
{
    const auto authored =
        world_model::ResolveAuthoredLocalLights(
            session.World().Objects(),
            root);

    std::vector<lighting::ResolvedLocalLight>
        result;
    result.reserve(
        authored.size());

    constexpr f64 kDegreesToRadians =
        0.017453292519943295769;

    for (const auto& item : authored)
    {
        const lighting::LocalLight light{
            .type =
                item.kind ==
                        world_model::
                            AuthoredLightKind::Spot
                    ? lighting::
                          LocalLightType::Spot
                    : lighting::
                          LocalLightType::Point,
            .positionInFrameMeters =
                item.positionMeters,
            .direction = {
                static_cast<f32>(
                    item.direction.x),
                static_cast<f32>(
                    item.direction.y),
                static_cast<f32>(
                    item.direction.z)
            },
            .colorLinear = {
                static_cast<f32>(
                    item.colorLinear.x),
                static_cast<f32>(
                    item.colorLinear.y),
                static_cast<f32>(
                    item.colorLinear.z)
            },
            .luminousFluxLumens =
                static_cast<f32>(
                    item.luminousFluxLumens),
            .rangeMeters =
                static_cast<f32>(
                    item.rangeMeters),
            .innerConeRadians =
                static_cast<f32>(
                    item.innerConeDegrees *
                    kDegreesToRadians),
            .outerConeRadians =
                static_cast<f32>(
                    item.outerConeDegrees *
                    kDegreesToRadians),
            .stableId =
                item.object.high ^
                item.object.low
        };

        result.push_back(
            lighting::ResolveLocalLight(
                light,
                lightingView));
    }

    return result;
}

[[nodiscard]] std::vector<scene::ObjectId>
CollectVolumeObjects(
    const scene::ObjectStore& objects)
{
    std::vector<scene::ObjectId>
        result;
    std::vector<scene::ObjectRecord>
        pending =
            objects.Roots();

    while (!pending.empty())
    {
        const auto current =
            pending.back();
        pending.pop_back();

        if (current.type ==
            world_model::kVolumeType)
        {
            result.push_back(
                current.id);
        }

        const auto children =
            objects.Children(
                current.id);

        pending.insert(
            pending.end(),
            children.begin(),
            children.end());
    }

    return result;
}

[[nodiscard]] std::vector<lighting::EmissiveVolumeSource>
ResolveAuthoredEmissiveVolumes(
    const scene::ObjectStore& objects)
{
    std::vector<lighting::EmissiveVolumeSource>
        result;

    constexpr u64 kEmissionBit =
        static_cast<u64>(
            world_model::
                VolumeField::Emission);

    for (const auto volumeId :
         CollectVolumeObjects(objects))
    {
        const auto domain =
            world_model::ResolveVolumeDomain(
                objects,
                volumeId);

        if (!domain.has_value() ||
            !domain->enabled ||
            !domain->renderEnabled ||
            (domain->fieldMask &
             kEmissionBit) == 0U ||
            domain->emissionScale <= 0.0F ||
            domain->giEmissionScale <= 0.0F)
        {
            continue;
        }

        f64 authoredEmission = 0.0;

        for (const auto& input :
             world_model::ResolveVolumeInputs(
                 objects,
                 volumeId))
        {
            if (!input.enabled ||
                input.role !=
                    world_model::
                        VolumeInputRole::Source ||
                (input.fieldMask &
                 kEmissionBit) == 0U)
            {
                continue;
            }

            authoredEmission +=
                std::max(
                    input.scalarValue,
                    0.0);
        }

        if (authoredEmission <= 0.0 &&
            domain->preset == "Fire")
        {
            authoredEmission = 1.0;
        }

        if (authoredEmission <= 0.0)
        {
            continue;
        }

        const auto source =
            volume_render::
                BuildEmissiveVolumeSource(
                    *domain,
                    static_cast<f32>(
                        authoredEmission));

        if (source.has_value())
        {
            result.push_back(
                *source);
        }
    }

    return result;
}

[[nodiscard]] editor_ui::PreviewMaterial
ResolveRuntimeMaterialAsset(
    content::ContentService& content,
    const content::AssetRecord* asset,
    const u32 depth)
{
    editor_ui::PreviewMaterial result{};

    if (asset == nullptr || depth > 8U)
    {
        return result;
    }

    if (asset->kind ==
            content::AssetKind::Material &&
        asset->material.has_value())
    {
        result.roughness =
            static_cast<f32>(
                std::clamp(
                    asset->material->
                        roughnessFactor,
                    0.0,
                    1.0));
        result.metallic =
            static_cast<f32>(
                std::clamp(
                    asset->material->
                        metallicFactor,
                    0.0,
                    1.0));
    }
    else if (
        asset->kind ==
            content::AssetKind::MaterialInstance &&
        asset->materialInstance.has_value())
    {
        const auto parentPath =
            asset->sourcePath.parent_path() /
            asset->materialInstance->parent;

        result =
            ResolveRuntimeMaterialAsset(
                content,
                content.FindByPath(
                    parentPath),
                depth + 1U);

        if (asset->materialInstance->
                roughnessFactor.has_value())
        {
            result.roughness =
                static_cast<f32>(
                    std::clamp(
                        *asset->materialInstance->
                            roughnessFactor,
                        0.0,
                        1.0));
        }

        if (asset->materialInstance->
                metallicFactor.has_value())
        {
            result.metallic =
                static_cast<f32>(
                    std::clamp(
                        *asset->materialInstance->
                            metallicFactor,
                        0.0,
                        1.0));
        }
    }
    else
    {
        return result;
    }

    const auto emission =
        content.ResolveMaterialEmission(
            asset->id);

    const auto evaluated =
        lighting::EvaluateMaterialEmission({
            .colorLinear = {
                static_cast<f32>(
                    emission.colorLinear[0]),
                static_cast<f32>(
                    emission.colorLinear[1]),
                static_cast<f32>(
                    emission.colorLinear[2])
            },
            .luminanceNits =
                static_cast<f32>(
                    emission.luminanceNits),
            .contributesToGi =
                emission.contributesToGi,
            .giScale =
                static_cast<f32>(
                    emission.giScale)
        });

    result.emissionRadiance =
        evaluated.visibleRadiance;
    result.emissionGiScale =
        emission.contributesToGi
            ? static_cast<f32>(
                  std::max(
                      emission.giScale,
                      0.0))
            : 0.0F;

    return result;
}

[[nodiscard]] std::optional<editor_ui::PreviewMaterial>
ResolveRuntimeBodyMaterialIfAssigned(
    content::ContentService* content,
    scene::ObjectStore& objects,
    const std::optional<scene::ObjectId> bodyObject)
{
    if (content == nullptr ||
        !bodyObject.has_value())
    {
        return std::nullopt;
    }

    const auto property =
        objects.GetProperty(
            *bodyObject,
            world_model::kBodyMaterialAsset);

    if (!property.has_value())
    {
        return std::nullopt;
    }

    const auto* textValue =
        std::get_if<std::string>(
            &*property);

    if (textValue == nullptr ||
        textValue->empty())
    {
        return std::nullopt;
    }

    const auto assetId =
        content::AssetId::Parse(
            *textValue);

    if (!assetId.has_value())
    {
        return std::nullopt;
    }

    const auto* asset =
        content->Find(*assetId);

    if (asset == nullptr ||
        (asset->kind != content::AssetKind::Material &&
         asset->kind != content::AssetKind::MaterialInstance))
    {
        return std::nullopt;
    }

    return ResolveRuntimeMaterialAsset(
        *content,
        asset);
}

[[nodiscard]] editor_ui::PreviewMaterial
ResolveRuntimeBodyMaterial(
    content::ContentService* content,
    scene::ObjectStore& objects,
    const std::optional<scene::ObjectId> bodyObject)
{
    if (content == nullptr ||
        !bodyObject.has_value())
    {
        return {};
    }

    const auto property =
        objects.GetProperty(
            *bodyObject,
            world_model::kBodyMaterialAsset);

    if (!property.has_value())
    {
        return {};
    }

    const auto* textValue =
        std::get_if<std::string>(
            &*property);

    if (textValue == nullptr ||
        textValue->empty())
    {
        return {};
    }

    const auto assetId =
        content::AssetId::Parse(
            *textValue);

    if (!assetId.has_value())
    {
        return {};
    }

    return ResolveRuntimeMaterialAsset(
        *content,
        content->Find(*assetId));
}
} // namespace orbit::studio_ui::viewport_detail
