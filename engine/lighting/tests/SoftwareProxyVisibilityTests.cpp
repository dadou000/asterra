#include <orbit/lighting/SoftwareProxyVisibility.hpp>

#include <array>
#include <cmath>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    frames::FrameGraph frames;
    const auto root =
        frames.CreateRoot();

    const auto child =
        frames.CreateFrame(
            root,
            [](time::SimulationTime)
            {
                return math::RigidTransformD{
                    .translation =
                        {0.0, 0.0, 5.0}
                };
            });

    const std::array proxies{
        VisibilityProxy{
            .stableId = 1U,
            .body = universe::BodyId{
                .high = 2U,
                .low = 3U},
            .frame = child,
            .frameFromProxy = {},
            .shape =
                VisibilityProxyShape::Sphere,
            .sphereRadiusMeters = 1.0,
            .materialId = 7U,
            .instanceId = 11U,
            .nominalErrorMeters = 0.1F
        },
        VisibilityProxy{
            .stableId = 2U,
            .body = universe::BodyId{
                .high = 2U,
                .low = 3U},
            .frame = root,
            .frameFromProxy = {
                .translation =
                    {3.0, 0.0, 8.0}
            },
            .shape =
                VisibilityProxyShape::Box,
            .boxHalfExtentsMeters =
                {1.0, 1.0, 1.0},
            .materialId = 9U,
            .instanceId = 13U,
            .nominalErrorMeters = 0.2F
        }
    };

    SoftwareProxyScene scene;
    scene.Rebuild(
        proxies,
        root,
        frames);

    if (scene.Stats().proxyCount != 2U ||
        scene.Stats().nodeCount == 0U)
    {
        return 1;
    }

    const auto hardwareAabbs =
        scene.AccelerationAabbs(
            {0.0, 0.0, 4.0});

    const auto hardwarePrimitives =
        scene.GpuPrimitives(
            {0.0, 0.0, 4.0});

    if (hardwareAabbs.size() != 2U ||
        hardwarePrimitives.size() != 2U ||
        std::abs(
            hardwarePrimitives[0].
                centerType.z -
            1.0F) >
            1.0e-6F ||
        hardwarePrimitives[0].
                materialId != 7U ||
        hardwarePrimitives[1].
                instanceId != 13U)
    {
        return 6;
    }

    SoftwareProxyVisibilityProvider
        provider(
            scene,
            {
                .maximumNodeVisits = 64U,
                .maximumPrimitiveTests = 16U
            });

    VisibilityRegistry registry;
    registry.Register(provider);

    VisibilityQuery query{
        .purpose =
            VisibilityPurpose::Reflection,
        .frame = root,
        .body = proxies[0].body,
        .originInFrameMeters =
            {0.0, 0.0, 0.0},
        .direction =
            {0.0F, 0.0F, 1.0F},
        .minimumDistanceMeters =
            0.01F,
        .maximumDistanceMeters =
            100.0F,
        .importance = 1.0F,
        .requirements = {
            .requireOffscreenCoverage =
                true,
            .requireSurfaceMaterial =
                true,
            .maximumNominalErrorMeters =
                0.5F,
            .minimumConfidence =
                0.5F
        }
    };

    const auto hit =
        registry.Trace(query);

    if (hit.resolution !=
            VisibilityResolution::Hit ||
        hit.backend !=
            VisibilityBackendKind::
                SoftwareProxy ||
        std::abs(
            hit.hit.distanceMeters -
            4.0F) >
            1.0e-5F ||
        hit.hit.materialId != 7U ||
        hit.hit.instanceId != 11U)
    {
        return 2;
    }

    query.direction =
        {0.0F, 1.0F, 0.0F};

    const auto miss =
        registry.Trace(query);

    if (miss.resolution !=
            VisibilityResolution::Miss ||
        !miss.terminal)
    {
        return 3;
    }

    query.direction =
        {0.0F, 0.0F, 1.0F};
    query.requirements.maximumNominalErrorMeters =
        0.05F;

    const auto lodRejected =
        registry.Trace(query);

    if (lodRejected.resolution !=
            VisibilityResolution::Unresolved ||
        lodRejected.terminal)
    {
        return 4;
    }

    query.requirements.maximumNominalErrorMeters =
        0.5F;

    SoftwareProxyVisibilityProvider
        starved(
            scene,
            {
                .maximumNodeVisits = 1U,
                .maximumPrimitiveTests = 1U
            });

    VisibilityRegistry starvedRegistry;
    starvedRegistry.Register(starved);

    query.direction =
        {1.0F, 0.0F, 0.0F};

    const auto starvedResult =
        starvedRegistry.Trace(query);

    if (starvedResult.resolution ==
            VisibilityResolution::Miss &&
        starvedResult.terminal)
    {
        return 5;
    }

    return 0;
}
