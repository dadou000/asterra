#include <orbit/path_geometry/CorridorDesignPreview.hpp>
#include <cmath>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " #x " line " << __LINE__ << '\n'; return 1; } } while(false)

int main()
{
    orbit::path_geometry::PathCenterline source{
        .edge = orbit::scene::ObjectId::Random(),
        .frame = orbit::frames::FrameId::Random(),
        .sourceRevision = 17U,
        .samples = {
            {.position={0,0,0},.up={0,1,0}},
            {.position={40,0,0},.up={0,1,0}}
        }};
    orbit::paths::PathProfile road{
        .name="Road",.kind=orbit::paths::PathProfileKind::Road,
        .widthMeters=6.0,.lanes=2,.minimumRadiusMeters=20.0,
        .maximumGrade=0.08
    };
    const auto preview = orbit::path_geometry::BuildCorridorDesignPreview(
        source,road,{},[](const orbit::math::Double3&) {return 1.0;});
    CHECK(preview.geometry.sourceRevision == 17U);
    CHECK(preview.geometry.stations.size() >= 2U);
    CHECK(preview.engineering.Valid());
    CHECK(preview.engineering.findings.empty());
    CHECK(preview.suggestedBankRadians.size() == preview.geometry.stations.size());
    CHECK(preview.earthworks.has_value());
    CHECK(std::abs(preview.earthworks->cutCubicMeters - 320.0) < 0.01);
    CHECK(preview.earthworks->fillCubicMeters == 0.0);
    const auto withoutTerrain = orbit::path_geometry::BuildCorridorDesignPreview(source,road);
    CHECK(!withoutTerrain.earthworks.has_value());
    CHECK(withoutTerrain.geometry.buildSignature == preview.geometry.buildSignature);
    return 0;
}
