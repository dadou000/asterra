#include <orbit/lighting/MaterialEmission.hpp>

#include <cmath>

int main()
{
    using namespace orbit::lighting;

    const auto enabled =
        EvaluateMaterialEmission({
            .colorLinear = {1.0F, 0.5F, 0.25F},
            .luminanceNits = 683.0F,
            .contributesToGi = true,
            .giScale = 2.0F
        });

    if (std::abs(enabled.visibleRadiance.x - 1.0F) >
            1.0e-6F ||
        std::abs(enabled.visibleRadiance.y - 0.5F) >
            1.0e-6F ||
        std::abs(enabled.giRadiance.x - 2.0F) >
            1.0e-6F)
    {
        return 1;
    }

    const auto disabled =
        EvaluateMaterialEmission({
            .colorLinear = {1.0F, 1.0F, 1.0F},
            .luminanceNits = 2000.0F,
            .contributesToGi = false,
            .giScale = 100.0F
        });

    if (disabled.visibleRadiance.x <= 0.0F ||
        disabled.giRadiance.x != 0.0F ||
        disabled.giRadiance.y != 0.0F ||
        disabled.giRadiance.z != 0.0F)
    {
        return 2;
    }

    orbit::lighting::SurfaceData surface;
    ApplyMaterialEmission(
        surface,
        {
            .colorLinear = {0.2F, 0.4F, 1.0F},
            .luminanceNits = 1366.0F,
            .contributesToGi = false,
            .giScale = 0.0F
        });

    if (surface.emissionRadianceSceneLinear.x <= 0.0F ||
        surface.emissionRadianceSceneLinear.z <=
            surface.emissionRadianceSceneLinear.x)
    {
        return 3;
    }

    SurfaceData surface;
    ApplyMaterialEmission(
        surface,
        {
            .colorLinear = {0.5F, 1.0F, 0.25F},
            .luminanceNits = 1366.0F,
            .contributesToGi = true,
            .giScale = 0.3F
        });

    if (surface.emissionRadianceSceneLinear.y <= 0.0F ||
        std::abs(
            surface.emissionGiScale -
            0.3F) >
            1.0e-6F)
    {
        return 3;
    }

    ApplyMaterialEmission(
        surface,
        {
            .colorLinear = {1.0F, 1.0F, 1.0F},
            .luminanceNits = 1000.0F,
            .contributesToGi = false,
            .giScale = 50.0F
        });

    if (surface.emissionRadianceSceneLinear.x <= 0.0F ||
        surface.emissionGiScale != 0.0F)
    {
        return 4;
    }

    const auto invalid =
        EvaluateMaterialEmission({
            .colorLinear = {-1.0F, 1.0F, 1.0F},
            .luminanceNits = -5.0F,
            .contributesToGi = true,
            .giScale = -3.0F
        });

    if (invalid.visibleRadiance.x != 0.0F ||
        invalid.visibleRadiance.y != 0.0F ||
        invalid.giRadiance.y != 0.0F)
    {
        return 4;
    }

    return 0;
}
