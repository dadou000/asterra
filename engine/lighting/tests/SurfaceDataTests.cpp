#include <orbit/lighting/SurfaceData.hpp>

#include <cmath>
#include <limits>

int main()
{
    using namespace orbit::lighting;

    SurfaceData source;
    source.geometricNormal = {0.0F, 4.0F, 0.0F};
    source.shadingNormal = {};
    source.baseColorLinear = {-1.0F, 0.5F, 2.0F};
    source.roughness = 2.0F;
    source.metallic = -1.0F;
    source.emissionRadianceSceneLinear = {
        25.0F,
        -3.0F,
        1200.0F
    };
    source.emissionGiScale = 2.5F;

    const auto canonical =
        Canonicalize(source);

    if (std::abs(canonical.geometricNormal.y - 1.0F) >
            1.0e-6F ||
        canonical.shadingNormal !=
            canonical.geometricNormal)
    {
        return 1;
    }

    if (canonical.baseColorLinear.x != 0.0F ||
        canonical.baseColorLinear.y != 0.5F ||
        canonical.baseColorLinear.z != 1.0F ||
        canonical.roughness != 1.0F ||
        canonical.metallic != 0.0F)
    {
        return 2;
    }

    // HDR emission must survive canonicalization. Only negative/non-finite
    // energy is rejected.
    if (canonical.emissionRadianceSceneLinear.x != 25.0F ||
        canonical.emissionRadianceSceneLinear.y != 0.0F ||
        canonical.emissionRadianceSceneLinear.z != 1200.0F ||
        canonical.emissionGiScale != 2.5F)
    {
        return 3;
    }

    source.emissionRadianceSceneLinear.x =
        std::numeric_limits<orbit::f32>::infinity();

    if (Canonicalize(source).
            emissionRadianceSceneLinear.x !=
        0.0F)
    {
        return 4;
    }

    if (!IsFinite(canonical))
    {
        return 5;
    }

    return 0;
}
