#include <orbit/celestial_appearance/PlanetaryAppearance.hpp>
#include <orbit/celestial_globe/MacroGlobe.hpp>

#include <cmath>

namespace
{
class TestTerrain final :
    public orbit::terrain::TerrainSource
{
public:
    explicit TestTerrain(
        const orbit::u64 revision)
        : revision_(revision)
    {
    }

    orbit::terrain::TerrainSample Sample(
        const orbit::terrain::TerrainQuery& query)
        const noexcept override
    {
        return {
            .elevationMeters =
                1000.0 *
                query.unitDirection.y,
            .coarseElevationMeters =
                1000.0 *
                query.unitDirection.y
        };
    }

    orbit::u64 Revision() const noexcept override
    {
        return revision_;
    }

private:
    orbit::u64 revision_{0};
};
} // namespace

int main()
{
    TestTerrain terrain(42);

    const auto mesh =
        orbit::celestial_globe::
            BuildMacroGlobe(
                terrain,
                orbit::universe::SphereShape{
                    .radiusMeters = 1.0e6
                },
                {
                    .faceResolution =
                        orbit::celestial_globe::
                            MacroGlobeResolution::Fixed(9U),
                    .footprintScale = 1.5
                });

    const auto appearance =
        orbit::celestial_appearance::
            BuildPlanetaryAppearance(
                terrain,
                1.0e6,
                {
                    .faceResolution = 9,
                    .footprintScale = 1.5
                });

    if (appearance.texels.size() !=
            mesh.vertices.size() ||
        appearance.faceResolution != 9U)
    {
        return 2;
    }

    const auto fingerprint =
        orbit::celestial_globe::
            MacroGlobeFingerprint(
                terrain,
                orbit::universe::SphereShape{
                    .radiusMeters = 1.0e6
                },
                {
                    .faceResolution =
                        orbit::celestial_globe::
                            MacroGlobeResolution::Fixed(9U),
                    .footprintScale = 1.5
                });

    if (fingerprint != mesh.fingerprint)
    {
        return 1;
    }

    if (mesh.vertices.size() !=
            6U * 9U * 9U ||
        mesh.indices.size() !=
            6U * 8U * 8U * 6U ||
        mesh.sourceRevision != 42 ||
        mesh.sampleFootprintMeters <= 0.0)
    {
        return 3;
    }

    if (mesh.minimumRadiusMeters >=
            1.0e6 ||
        mesh.maximumRadiusMeters <=
            1.0e6)
    {
        return 4;
    }

    for (const auto& vertex :
         mesh.vertices)
    {
        const double normalLength =
            orbit::math::Length(
                vertex.normal);

        if (!std::isfinite(normalLength) ||
            std::abs(
                normalLength - 1.0) >
                1.0e-9)
        {
            return 5;
        }
    }

    // Orbit's Vulkan rasterizer treats clockwise screen-space winding as the
    // exterior. In body space that is the opposite of the conventional
    // mathematical outward cross product for this projection convention.
    for (std::size_t index = 0; index < mesh.indices.size(); index += 3U)
    {
        const auto& a = mesh.vertices[mesh.indices[index + 0U]].positionMeters;
        const auto& b = mesh.vertices[mesh.indices[index + 1U]].positionMeters;
        const auto& c = mesh.vertices[mesh.indices[index + 2U]].positionMeters;
        if (orbit::math::Dot(orbit::math::Cross(b - a, c - a), a) >= 0.0)
        {
            return 8;
        }
    }

    TestTerrain revised(43);

    const auto revisedMesh =
        orbit::celestial_globe::
            BuildMacroGlobe(
                revised,
                orbit::universe::SphereShape{
                    .radiusMeters = 1.0e6
                },
                {
                    .faceResolution =
                        orbit::celestial_globe::
                            MacroGlobeResolution::Fixed(9U),
                    .footprintScale = 1.5
                });

    if (revisedMesh.fingerprint ==
        mesh.fingerprint)
    {
        return 6;
    }

    const auto ellipsoid =
        orbit::celestial_globe::
            BuildMacroGlobe(
                terrain,
                orbit::universe::EllipsoidShape{
                    .radiiMeters = {
                        1.0e6,
                        0.9e6,
                        0.8e6
                    }
                },
                {
                    .faceResolution =
                        orbit::celestial_globe::
                            MacroGlobeResolution::Fixed(5U),
                    .footprintScale = 1.0
                });

    if (ellipsoid.fingerprint ==
            mesh.fingerprint ||
        ellipsoid.maximumRadiusMeters <=
            ellipsoid.minimumRadiusMeters)
    {
        return 7;
    }

    return 0;
}
