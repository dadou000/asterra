#include <orbit/lighting/SurfaceBuffer.hpp>

#include <array>

int main()
{
    using namespace orbit::lighting;

    constexpr std::array classes{
        SurfaceClass::Unknown,
        SurfaceClass::Terrain,
        SurfaceClass::Water,
        SurfaceClass::CelestialSurface,
        SurfaceClass::Procedural
    };

    constexpr std::array representations{
        SurfaceRepresentation::Unknown,
        SurfaceRepresentation::ProductionSurface,
        SurfaceRepresentation::MacroGlobe,
        SurfaceRepresentation::CachedImpostor,
        SurfaceRepresentation::ProceduralProxy
    };

    for (const auto surfaceClass : classes)
    {
        for (const auto representation : representations)
        {
            const auto decoded =
                DecodeSurfaceMetadata(
                    EncodeSurfaceMetadata(
                        surfaceClass,
                        representation));

            if (decoded.surfaceClass != surfaceClass ||
                decoded.representation != representation)
            {
                return 1;
            }
        }
    }

    if (DecodeSurfaceMetadata(-1.0F).surfaceClass !=
        SurfaceClass::Unknown)
    {
        return 2;
    }

    return 0;
}
