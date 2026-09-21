#include <orbit/lighting/AnalyticBodyVisibility.hpp>

#include <cmath>

int main()
{
    using namespace orbit;

    frames::FrameGraph frames;
    const auto root =
        frames.CreateRoot();

    universe::BodyRegistry bodies(frames);
    const auto system =
        bodies.CreateSystem(
            "Test System");

    const auto body =
        bodies.CreateBody({
            .system = system,
            .name = "Sphere",
            .parentFrame = root,
            .shape =
                universe::SphereShape{
                    .radiusMeters = 10.0},
            .transformModel =
                universe::FixedBodyTransform{
                    .parentFromBody =
                        math::IdentityRigidTransformD()}
        });

    const auto* bodyRecord =
        bodies.FindBody(body);

    if (bodyRecord == nullptr)
    {
        return 1;
    }

    lighting::AnalyticBodyVisibilityProvider
        provider(
            bodies,
            frames);

    lighting::VisibilityQuery query{
        .purpose =
            lighting::VisibilityPurpose::
                Diagnostic,
        .frame =
            bodyRecord->frame,
        .body =
            body,
        .originInFrameMeters =
            {0.0, 0.0, 20.0},
        .direction =
            {0.0F, 0.0F, -1.0F},
        .minimumDistanceMeters =
            0.01F,
        .maximumDistanceMeters =
            100.0F,
        .importance = 1.0F,
        .requirements = {
            .requireOffscreenCoverage =
                true,
            .requirePlanetaryRange =
                true
        }
    };

    const auto hit =
        provider.Trace(query);

    if (hit.resolution !=
            lighting::VisibilityResolution::Hit ||
        std::abs(
            hit.hit.distanceMeters -
            10.0F) >
            1.0e-4F ||
        std::abs(
            hit.hit.positionInFrameMeters.z -
            10.0) >
            1.0e-8 ||
        hit.backend !=
            lighting::VisibilityBackendKind::
                Unknown)
    {
        // Provider provenance is attached by VisibilityRegistry, not by the
        // provider itself.
        return 2;
    }

    lighting::VisibilityRegistry registry;
    registry.Register(provider);

    const auto registeredHit =
        registry.Trace(query);

    if (registeredHit.providerId !=
            provider.Description().providerId ||
        registeredHit.backend !=
            lighting::VisibilityBackendKind::
                Analytic ||
        registeredHit.providerName !=
            "Analytic Body")
    {
        return 3;
    }

    query.direction =
        {0.0F, 1.0F, 0.0F};

    const auto miss =
        registry.Trace(query);

    if (miss.resolution !=
            lighting::VisibilityResolution::
                Unresolved ||
        miss.terminal)
    {
        return 4;
    }

    return 0;
}
