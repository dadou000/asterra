#include <orbit/lighting/Visibility.hpp>

#include <stdexcept>
#include <utility>

namespace
{
class MockProvider final
    : public orbit::lighting::VisibilityProvider
{
public:
    MockProvider(
        orbit::lighting::VisibilityProviderDesc desc,
        orbit::lighting::VisibilityResult result)
        : desc_(std::move(desc)),
          result_(std::move(result))
    {
    }

    [[nodiscard]] const orbit::lighting::
        VisibilityProviderDesc&
    Description() const noexcept override
    {
        return desc_;
    }

    [[nodiscard]] bool SupportsPurpose(
        const orbit::lighting::
            VisibilityPurpose) const noexcept override
    {
        return true;
    }

    [[nodiscard]] orbit::lighting::
        VisibilityResult Trace(
            const orbit::lighting::
                VisibilityQuery&) override
    {
        ++traceCount;
        return result_;
    }

    int traceCount{0};

private:
    orbit::lighting::VisibilityProviderDesc desc_;
    orbit::lighting::VisibilityResult result_;
};
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    MockProvider screen(
        {
            .providerId = 1U,
            .name = "screen",
            .kind =
                VisibilityBackendKind::ScreenSpace,
            .capabilities =
                VisibilityCapability::ViewDependent |
                VisibilityCapability::DynamicGeometry |
                VisibilityCapability::SurfaceMaterial,
            .nominalErrorMeters = 0.02F,
            .priority = 100
        },
        {
            .resolution =
                VisibilityResolution::Unresolved,
            .confidence = 0.9F,
            .terminal = false
        });

    MockProvider analytic(
        {
            .providerId = 2U,
            .name = "analytic",
            .kind =
                VisibilityBackendKind::Analytic,
            .capabilities =
                VisibilityCapability::Offscreen |
                VisibilityCapability::PlanetaryRange,
            .nominalErrorMeters = 1.0F,
            .priority = 80
        },
        {
            .resolution =
                VisibilityResolution::Hit,
            .hit = {
                .distanceMeters = 42.0F
            },
            .confidence = 1.0F,
            .terminal = true
        });

    MockProvider exact(
        {
            .providerId = 3U,
            .name = "exact-scene",
            .kind =
                VisibilityBackendKind::SoftwareProxy,
            .capabilities =
                VisibilityCapability::Offscreen |
                VisibilityCapability::ExactGeometry |
                VisibilityCapability::DynamicGeometry |
                VisibilityCapability::SurfaceMaterial,
            .nominalErrorMeters = 0.001F,
            .priority = 60
        },
        {
            .resolution =
                VisibilityResolution::Miss,
            .confidence = 1.0F,
            .terminal = true
        });

    VisibilityRegistry registry;
    registry.Register(screen);
    registry.Register(analytic);
    registry.Register(exact);

    VisibilityQuery query{
        .purpose = VisibilityPurpose::DiffuseGi,
        .frame = frames::FrameId{
            .high = 1U,
            .low = 2U},
        .body = universe::BodyId{
            .high = 3U,
            .low = 4U},
        .originInFrameMeters = {},
        .direction = {0.0F, 0.0F, 1.0F},
        .minimumDistanceMeters = 0.01F,
        .maximumDistanceMeters = 1000.0F,
        .importance = 1.0F
    };

    VisibilityTraceDiagnostics diagnostics;
    const auto result =
        registry.Trace(
            query,
            &diagnostics);

    if (result.resolution !=
            VisibilityResolution::Hit ||
        result.providerId != 2U ||
        diagnostics.attempts.size() != 2U ||
        screen.traceCount != 1 ||
        analytic.traceCount != 1 ||
        exact.traceCount != 0)
    {
        return 1;
    }

    query.requirements.requireExactGeometry =
        true;
    query.requirements.requireOffscreenCoverage =
        true;
    query.requirements.maximumNominalErrorMeters =
        0.01F;

    const auto candidates =
        registry.CandidateProviders(query);

    if (candidates.size() != 1U ||
        candidates.front().providerId != 3U)
    {
        return 2;
    }

    const auto exactResult =
        registry.Trace(query);

    if (exactResult.resolution !=
            VisibilityResolution::Miss ||
        exactResult.backend !=
            VisibilityBackendKind::SoftwareProxy)
    {
        return 3;
    }

    bool duplicateRejected = false;
    try
    {
        registry.Register(exact);
    }
    catch (const std::invalid_argument&)
    {
        duplicateRejected = true;
    }

    if (!duplicateRejected)
    {
        return 4;
    }

    return 0;
}
