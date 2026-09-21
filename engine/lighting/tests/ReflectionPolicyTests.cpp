#include <orbit/lighting/ReflectionPolicy.hpp>

int main()
{
    using namespace orbit::lighting;

    const auto screen =
        PlanReflection(
            {
                .roughness = 0.02F,
                .screenConfidence = 0.95F,
                .importance = 1.0F,
                .screenHit = true
            },
            true,
            true);

    if (!screen.useScreenHit ||
        screen.requestExactVisibility)
    {
        return 1;
    }

    const auto mirrorRt =
        PlanReflection(
            {
                .roughness = 0.02F,
                .screenConfidence = 0.0F,
                .importance = 1.0F,
                .screenHit = false
            },
            true,
            true);

    if (!mirrorRt.requestExactVisibility ||
        mirrorRt.fallback !=
            ReflectionFallback::
                HardwareRayQuery)
    {
        return 2;
    }

    const auto mirrorSoftware =
        PlanReflection(
            {
                .roughness = 0.02F,
                .importance = 1.0F
            },
            false,
            true);

    if (!mirrorSoftware.requestExactVisibility ||
        mirrorSoftware.fallback !=
            ReflectionFallback::
                SoftwareExact)
    {
        return 3;
    }

    const auto mirrorNoExact =
        PlanReflection(
            {
                .roughness = 0.02F,
                .importance = 1.0F
            },
            false,
            false);

    if (!mirrorNoExact.useRadianceCache ||
        mirrorNoExact.requestExactVisibility)
    {
        return 4;
    }

    const auto rough =
        PlanReflection(
            {
                .roughness = 0.8F,
                .importance = 1.0F
            },
            true,
            true);

    if (!rough.useRadianceCache ||
        rough.requestExactVisibility)
    {
        return 5;
    }

    const auto unimportantMirror =
        PlanReflection(
            {
                .roughness = 0.01F,
                .importance = 0.01F
            },
            true,
            true);

    if (!unimportantMirror.useRadianceCache ||
        unimportantMirror.requestExactVisibility)
    {
        return 6;
    }

    return 0;
}
