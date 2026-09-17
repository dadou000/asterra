#include <orbit/surface_authoring/TerrainConstraints.hpp>

#include <cmath>
#include <iostream>
#include <string>

namespace
{
[[nodiscard]] bool Check(
    const bool condition,
    const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

[[nodiscard]] bool Near(
    const orbit::f64 left,
    const orbit::f64 right,
    const orbit::f64 epsilon = 1.0e-5)
{
    return std::abs(left - right) <= epsilon;
}

[[nodiscard]] orbit::surface_authoring::TerrainConstraintId Id(
    const orbit::u64 low)
{
    return {
        .high = 0x4F524249544D3034ULL,
        .low = low
    };
}
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::surface_authoring;

    const world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0,
        .id = {
            .high = 0x504C414E45544D30ULL,
            .low = 0x3400000000000001ULL
        },
        .generationSeed = 12345
    };

    const terrain::PlanetSurfacePosition origin{
        .planet = planet.id,
        .unitDirection = {1.0, 0.0, 0.0},
        .radialOffsetMeters = 0.0
    };

    const auto west = terrain::OffsetSurfacePosition(
        planet, origin, {-600.0, 0.0});
    const auto east = terrain::OffsetSurfacePosition(
        planet, origin, {600.0, 0.0});
    const auto north = terrain::OffsetSurfacePosition(
        planet, origin, {0.0, 1'000.0});

    const SplineConstraintPrimitive canyon{
        .controlUnitDirections = {
            west.unitDirection,
            origin.unitDirection,
            east.unitDirection
        },
        .halfWidthMeters = 80.0,
        .falloffMeters = 120.0
    };

    TerrainConstraintSet authored{
        .id = {
            .high = 0x4F52424954544353ULL,
            .low = 0x4554000000000001ULL
        },
        .planet = planet.id,
        .name = "M04 Canyon Acceptance"
    };

    authored.height.constraints.push_back({
        .id = Id(1),
        .mode = ConstraintCompositionMode::Add,
        .primitive = canyon,
        .value = -150.0,
        .opacity = 1.0,
        .enabled = true
    });
    authored.protection.constraints.push_back({
        .id = Id(2),
        .mode = ConstraintCompositionMode::Replace,
        .primitive = canyon,
        .value = 0.60,
        .opacity = 1.0,
        .enabled = true
    });
    authored.drainage.constraints.push_back({
        .id = Id(3),
        .mode = ConstraintCompositionMode::Add,
        .primitive = canyon,
        .value = 1.0,
        .opacity = 1.0,
        .enabled = true
    });
    authored.material.constraints.push_back({
        .id = Id(4),
        .mode = ConstraintCompositionMode::Replace,
        .primitive = canyon,
        .material = terrain_geology::reference_rock::Basalt,
        .weight = 1.0,
        .opacity = 1.0,
        .enabled = true
    });

    bool ok = true;
    ok &= Check(authored.IsValid(),
        "Authored M04 canyon constraint set should validate.");

    const TerrainConstraintBaseline baseline{
        .heightMeters = 1'000.0,
        .gradient = {},
        .upliftMeters = 0.0,
        .material = terrain_geology::reference_rock::Sandstone,
        .protection = 0.0,
        .drainage = 0.0
    };

    const TerrainConstraintSample inside =
        EvaluateTerrainConstraintSet(
            authored, planet, origin, baseline);
    const TerrainConstraintSample outside =
        EvaluateTerrainConstraintSet(
            authored, planet, north, baseline);

    ok &= Check(
        Near(inside.heightMeters, 850.0, 1.0e-3),
        "Spline-authored canyon must carve the derived height field.");
    ok &= Check(
        Near(outside.heightMeters, 1'000.0, 1.0e-3),
        "Canyon must not modify terrain outside its authored falloff.");
    ok &= Check(
        inside.protection > 0.59 && inside.protection < 0.61,
        "Canyon preservation field must be evaluated independently of height.");
    ok &= Check(
        ErosionAllowanceFromProtection(inside.protection) > 0.39 &&
        ErosionAllowanceFromProtection(inside.protection) < 0.41,
        "Protected authored terrain must still permit partial erosion.");
    ok &= Check(
        Near(ErosionAllowanceFromProtection(outside.protection), 1.0),
        "Unprotected terrain must retain full erosion allowance.");
    ok &= Check(
        inside.drainage > 0.99 && outside.drainage < 0.01,
        "The same authored spline must be reusable for drainage constraints.");
    ok &= Check(
        inside.material.count >= 1 &&
        inside.material.materials[0] ==
            terrain_geology::reference_rock::Basalt,
        "Material constraints must participate in the unified authored field set.");

    // Regeneration acceptance: persist only authored intent, reload it, and
    // derive the canyon again. No generated height page is serialized here.
    const std::string serialized =
        SerializeTerrainConstraintSetToml(authored);
    const TerrainConstraintSet reloaded =
        ParseTerrainConstraintSetToml(serialized);
    const TerrainConstraintSample regenerated =
        EvaluateTerrainConstraintSet(
            reloaded, planet, origin, baseline);

    ok &= Check(
        Near(regenerated.heightMeters, inside.heightMeters) &&
        Near(regenerated.protection, inside.protection) &&
        Near(regenerated.drainage, inside.drainage),
        "A spline canyon must survive regeneration from project authority.");

    // Point, brush, polygon and raster primitives are all real evaluators.
    const PointConstraintPrimitive point{
        .centerUnitDirection = origin.unitDirection,
        .radiusMeters = 200.0
    };
    const BrushConstraintPrimitive brush{
        .centerUnitDirection = origin.unitDirection,
        .innerRadiusMeters = 50.0,
        .outerRadiusMeters = 250.0
    };

    const auto southwest = terrain::OffsetSurfacePosition(
        planet, origin, {-200.0, -200.0});
    const auto southeast = terrain::OffsetSurfacePosition(
        planet, origin, {200.0, -200.0});
    const auto northeast = terrain::OffsetSurfacePosition(
        planet, origin, {200.0, 200.0});
    const auto northwest = terrain::OffsetSurfacePosition(
        planet, origin, {-200.0, 200.0});

    const PolygonConstraintPrimitive polygon{
        .verticesUnitDirections = {
            southwest.unitDirection,
            southeast.unitDirection,
            northeast.unitDirection,
            northwest.unitDirection
        },
        .falloffMeters = 100.0
    };
    const RasterMaskConstraintPrimitive raster{
        .anchorUnitDirection = origin.unitDirection,
        .rotationRadians = 0.0,
        .width = 3,
        .height = 3,
        .cellSizeMeters = 100.0,
        .samples = {
            0.0F, 0.0F, 0.0F,
            0.0F, 1.0F, 0.0F,
            0.0F, 0.0F, 0.0F
        }
    };

    ok &= Check(
        EvaluateConstraintInfluence(point, planet, origin) > 0.999,
        "Point primitive must evaluate at its canonical center.");
    ok &= Check(
        EvaluateConstraintInfluence(brush, planet, origin) > 0.999,
        "Brush primitive must evaluate its inner plateau.");
    ok &= Check(
        EvaluateConstraintInfluence(polygon, planet, origin) > 0.999,
        "Polygon primitive must evaluate its authored interior.");
    ok &= Check(
        EvaluateConstraintInfluence(raster, planet, origin) > 0.999,
        "Imported raster primitive must bilinearly evaluate its mask.");

    // All six composition modes have deterministic scalar semantics.
    TerrainConstraintSet modes{
        .id = {
            .high = 0x4F52424954544353ULL,
            .low = 0x4554000000000002ULL
        },
        .planet = planet.id,
        .name = "M04 composition modes"
    };

    const TerrainConstraintPrimitive fullPoint = point;
    const std::array<ConstraintCompositionMode, 6> modeOrder{
        ConstraintCompositionMode::Add,
        ConstraintCompositionMode::Subtract,
        ConstraintCompositionMode::Multiply,
        ConstraintCompositionMode::Min,
        ConstraintCompositionMode::Max,
        ConstraintCompositionMode::Replace
    };
    const std::array<f64, 6> values{
        5.0, 2.0, 2.0, 20.0, 30.0, 42.0
    };

    for (std::size_t index = 0;
         index < modeOrder.size();
         ++index)
    {
        modes.height.constraints.push_back({
            .id = Id(100 + index),
            .mode = modeOrder[index],
            .primitive = fullPoint,
            .value = values[index],
            .opacity = 1.0,
            .enabled = true
        });
    }

    const TerrainConstraintSample composed =
        EvaluateTerrainConstraintSet(
            modes,
            planet,
            origin,
            TerrainConstraintBaseline{
                .heightMeters = 10.0
            });

    ok &= Check(
        Near(composed.heightMeters, 42.0, 1.0e-4),
        "Add/Subtract/Multiply/Min/Max/Replace composition must be deterministic.");

    // Constraint geometry is canonical, not cube-face local. Represent the
    // exact +X/+Z seam point through either face and require identical output.
    const world::CubeCoordinate plusX{
        .face = world::CubeFace::PositiveX,
        .uv = {-1.0, 0.0}
    };
    const world::CubeCoordinate plusZ{
        .face = world::CubeFace::PositiveZ,
        .uv = {1.0, 0.0}
    };
    const auto seamX = terrain::SurfacePositionFromCube(
        planet.id, plusX);
    const auto seamZ = terrain::SurfacePositionFromCube(
        planet.id, plusZ);

    const PointConstraintPrimitive seamPoint{
        .centerUnitDirection = seamX.unitDirection,
        .radiusMeters = 500.0
    };
    ok &= Check(
        Near(
            EvaluateConstraintInfluence(seamPoint, planet, seamX),
            EvaluateConstraintInfluence(seamPoint, planet, seamZ),
            1.0e-10),
        "Authored constraints must have one phase across cube-face seams.");

    return ok ? 0 : 1;
}
