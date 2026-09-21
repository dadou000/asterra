#include <orbit/lighting/EmissiveMaterial.hpp>

#include <cmath>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    const PhysicalEmissionMaterial white{
        .colorLinear = {1.0F, 1.0F, 1.0F},
        .luminanceNits = 1000.0F,
        .contributesToGi = true,
        .giScale = 0.5F
    };

    const auto evaluated =
        EvaluatePhysicalEmission(white);

    if (!(evaluated.visibleRadianceSceneLinear.x > 0.0F) ||
        std::abs(
            evaluated.visibleRadianceSceneLinear.x -
            evaluated.visibleRadianceSceneLinear.y) >
            1.0e-7F ||
        std::abs(
            evaluated.giRadianceSceneLinear.x -
            evaluated.visibleRadianceSceneLinear.x * 0.5F) >
            1.0e-7F)
    {
        return 1;
    }

    // Equal authored luminance must remain equal after changing hue.
    const auto red =
        EvaluatePhysicalEmission({
            .colorLinear = {1.0F, 0.0F, 0.0F},
            .luminanceNits = 1000.0F,
            .contributesToGi = true,
            .giScale = 1.0F
        });

    const f32 redY =
        red.visibleRadianceSceneLinear.x * 0.2126F +
        red.visibleRadianceSceneLinear.y * 0.7152F +
        red.visibleRadianceSceneLinear.z * 0.0722F;

    const f32 whiteY =
        evaluated.visibleRadianceSceneLinear.x * 0.2126F +
        evaluated.visibleRadianceSceneLinear.y * 0.7152F +
        evaluated.visibleRadianceSceneLinear.z * 0.0722F;

    if (std::abs(redY - whiteY) > 1.0e-6F)
    {
        return 2;
    }

    const auto noGi =
        EvaluatePhysicalEmission({
            .colorLinear = {0.2F, 1.0F, 0.4F},
            .luminanceNits = 5000.0F,
            .contributesToGi = false,
            .giScale = 100.0F
        });

    if (!(noGi.visibleRadianceSceneLinear.y > 0.0F) ||
        noGi.giRadianceSceneLinear !=
            math::Float3{})
    {
        return 3;
    }

    const auto textured =
        EvaluatePhysicalEmission(
            {
                .colorLinear = {1.0F, 1.0F, 1.0F},
                .luminanceNits = 2000.0F
            },
            {1.0F, 0.0F, 0.0F});

    if (!(textured.visibleRadianceSceneLinear.x > 0.0F) ||
        textured.visibleRadianceSceneLinear.y != 0.0F ||
        textured.visibleRadianceSceneLinear.z != 0.0F)
    {
        return 4;
    }

    return 0;
}
