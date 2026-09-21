#include <orbit/lighting/SkyVisibility.hpp>

#include <utility>

namespace
{
class ConstantProvider final
    : public orbit::lighting::VisibilityProvider
{
public:
    ConstantProvider(
        orbit::u64 id,
        orbit::lighting::VisibilityResolution resolution)
        : resolution_(resolution),
          desc_({
              .providerId = id,
              .name = "constant",
              .kind =
                  orbit::lighting::
                      VisibilityBackendKind::SoftwareProxy,
              .capabilities =
                  orbit::lighting::
                      VisibilityCapability::Offscreen,
              .nominalErrorMeters = 0.0F,
              .priority = 1
          })
    {
    }

    [[nodiscard]] const orbit::lighting::
        VisibilityProviderDesc&
    Description() const noexcept override
    {
        return desc_;
    }

    [[nodiscard]] bool SupportsPurpose(
        orbit::lighting::VisibilityPurpose) const noexcept override
    {
        return true;
    }

    [[nodiscard]] orbit::lighting::VisibilityResult Trace(
        const orbit::lighting::VisibilityQuery&) override
    {
        return {
            .resolution = resolution_,
            .confidence = 1.0F,
            .terminal = true
        };
    }

private:
    orbit::lighting::VisibilityResolution resolution_;
    orbit::lighting::VisibilityProviderDesc desc_;
};
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    ConstantProvider open(
        1U,
        VisibilityResolution::Miss);

    VisibilityRegistry openRegistry;
    openRegistry.Register(open);

    const auto openSky =
        EstimateSkyVisibility(
            openRegistry,
            frames::FrameId{
                .high = 1U,
                .low = 2U},
            universe::BodyId{
                .high = 3U,
                .low = 4U},
            {0.0, 10.0, 0.0},
            {0.0F, 1.0F, 0.0F});

    if (openSky.visibleFraction < 0.999F ||
        openSky.openDirection.y <= 0.0F)
    {
        return 1;
    }

    ConstantProvider blocked(
        2U,
        VisibilityResolution::Hit);

    VisibilityRegistry blockedRegistry;
    blockedRegistry.Register(blocked);

    const auto blockedSky =
        EstimateSkyVisibility(
            blockedRegistry,
            frames::FrameId{
                .high = 1U,
                .low = 2U},
            universe::BodyId{
                .high = 3U,
                .low = 4U},
            {0.0, 10.0, 0.0},
            {0.0F, 1.0F, 0.0F});

    if (blockedSky.visibleFraction != 0.0F)
    {
        return 2;
    }

    return 0;
}
