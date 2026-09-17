#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_geology/Stratigraphy.hpp>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR
#error ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR must be defined for M03 tests.
#endif

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

[[nodiscard]] bool NearlyEqual(
    const orbit::f64 a,
    const orbit::f64 b,
    const orbit::f64 epsilon = 1.0e-8)
{
    return std::abs(a - b) <= epsilon;
}
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::terrain;
    using namespace orbit::terrain_geology;
    using namespace orbit::world;

    const std::filesystem::path referenceDirectory{
        ORBIT_TERRAIN_GEOLOGY_REFERENCE_ASSET_DIR
    };

    const GeologicalMaterialLibrary materials =
        LoadGeologicalMaterialDirectory(
            referenceDirectory);

    const StratigraphyProfile authored =
        LoadStratigraphyProfileFile(
            referenceDirectory /
            "continental_shelf.orbitstratigraphy");

    bool ok = true;

    ok &= Check(
        authored.IsValid(),
        "Authored M03 reference stratigraphy must validate.");
    ok &= Check(
        ReferencesKnownMaterials(
            authored,
            materials),
        "M03 profile must resolve every RockTypeId through the M02 material library.");
    ok &= Check(
        authored.layers.size() == 3,
        "Reference stratigraphy must contain three finite conceptual layers.");

    const CompiledStratigraphyProfile compiled{
        authored,
        materials
    };

    ok &= Check(
        NearlyEqual(
            compiled.LayerTopDepthMeters(0),
            0.0) &&
        NearlyEqual(
            compiled.LayerBottomDepthMeters(0),
            2.0) &&
        NearlyEqual(
            compiled.LayerBottomDepthMeters(1),
            7.0) &&
        NearlyEqual(
            compiled.LayerBottomDepthMeters(2),
            15.0),
        "M03 cumulative layer depth/thickness evaluation is incorrect.");

    const PlanetDefinition planet{
        .radiusMeters = 6'000'000.0,
        .id = {
            .high = 0xA57E220000000003ULL,
            .low = 0x0000000000000003ULL
        },
        .generationSeed = 0x03030303ULL
    };

    const PlanetSurfacePosition anchor{
        .planet = planet.id,
        .unitDirection =
            math::Normalize(
                authored.transform.anchorUnitDirection),
        .radialOffsetMeters = 0.0
    };

    const f64 topAtAnchor =
        compiled.DeformedTopRadialOffsetMeters(
            planet,
            anchor);

    ok &= Check(
        std::isfinite(topAtAnchor),
        "M03 deformed virtual top must evaluate to a finite body-space offset.");

    const auto SampleAtDepth =
        [&](const f64 depthMeters)
        {
            PlanetSurfacePosition exposed =
                anchor;
            exposed.radialOffsetMeters =
                topAtAnchor - depthMeters;
            return compiled.SampleExposedLayer(
                planet,
                exposed);
        };

    const StratigraphySample shallow =
        SampleAtDepth(1.0);
    const StratigraphySample middle =
        SampleAtDepth(3.0);
    const StratigraphySample deep =
        SampleAtDepth(8.0);
    const StratigraphySample basement =
        SampleAtDepth(16.0);

    ok &= Check(
        shallow.primaryMaterial ==
            reference_rock::VolcanicAsh &&
        middle.primaryMaterial ==
            reference_rock::Sandstone &&
        deep.primaryMaterial ==
            reference_rock::Limestone &&
        basement.primaryMaterial ==
            reference_rock::Basalt &&
        basement.basement,
        "Lowering the exposed surface must walk through virtual layers into basement.");

    const GeologicalMaterial* ash =
        materials.Find(
            shallow.primaryMaterial);
    const GeologicalMaterial* basalt =
        materials.Find(
            basement.primaryMaterial);

    ok &= Check(
        ash != nullptr &&
            basalt != nullptr &&
            basalt->hardness > ash->hardness &&
            basalt->hydraulicErodibility <
                ash->hydraulicErodibility,
        "Erosion through the weak upper layer must expose the harder M02 rock below.");

    const StratigraphySample transition =
        SampleAtDepth(1.9);

    ok &= Check(
        transition.primaryMaterial ==
            reference_rock::VolcanicAsh &&
        transition.secondaryMaterial ==
            reference_rock::Sandstone &&
        transition.secondaryWeight > 0.0F &&
        transition.secondaryWeight < 1.0F,
        "M03 optional material transition band must blend toward the next virtual layer.");

    // The exact +X/+Z cube-face seam is the same physical point expressed by
    // two storage projections. Stratigraphy must therefore be exactly phase
    // coherent there because it samples canonical body-space position only.
    const PlanetSurfacePosition seamFromX =
        SurfacePositionFromCube(
            planet.id,
            {
                .face = CubeFace::PositiveX,
                .uv = {-1.0, 0.0}
            });
    const PlanetSurfacePosition seamFromZ =
        SurfacePositionFromCube(
            planet.id,
            {
                .face = CubeFace::PositiveZ,
                .uv = {1.0, 0.0}
            });

    const f64 seamX =
        compiled.DeformedTopRadialOffsetMeters(
            planet,
            seamFromX);
    const f64 seamZ =
        compiled.DeformedTopRadialOffsetMeters(
            planet,
            seamFromZ);

    ok &= Check(
        NearlyEqual(seamX, seamZ, 1.0e-7),
        "Fold/tilt/warp evaluation changed phase across equivalent cube-face seam coordinates.");

    // Sample a few meters on either side of that face boundary. These resolve
    // to different physical tile faces but the virtual boundary should change
    // smoothly, not jump/reset as a per-face 2D field would.
    const PlanetSurfacePosition nearX =
        SurfacePositionFromCube(
            planet.id,
            {
                .face = CubeFace::PositiveX,
                .uv = {-0.999999, 0.0}
            });
    const PlanetSurfacePosition nearZ =
        SurfacePositionFromCube(
            planet.id,
            {
                .face = CubeFace::PositiveZ,
                .uv = {0.999999, 0.0}
            });

    const PlanetTileId tileX =
        PhysicalTileForPosition(
            nearX,
            12);
    const PlanetTileId tileZ =
        PhysicalTileForPosition(
            nearZ,
            12);

    const f64 topNearX =
        compiled.DeformedTopRadialOffsetMeters(
            planet,
            nearX);
    const f64 topNearZ =
        compiled.DeformedTopRadialOffsetMeters(
            planet,
            nearZ);

    ok &= Check(
        tileX.face != tileZ.face,
        "M03 seam test setup must span two different cube-face tiles.");
    ok &= Check(
        std::abs(topNearX - topNearZ) < 1.0,
        "Folded/tilted/warped strata must remain continuous when crossing physical tile faces.");

    const std::string serialized =
        SerializeStratigraphyProfileToml(
            authored);
    const StratigraphyProfile roundTrip =
        ParseStratigraphyProfileToml(
            serialized);

    ok &= Check(
        roundTrip.id == authored.id &&
        roundTrip.name == authored.name &&
        roundTrip.basementMaterial ==
            authored.basementMaterial &&
        roundTrip.layers.size() ==
            authored.layers.size() &&
        NearlyEqual(
            roundTrip.transform.foldAmplitudeMeters,
            authored.transform.foldAmplitudeMeters) &&
        NearlyEqual(
            roundTrip.layers[1].thicknessMeters,
            authored.layers[1].thicknessMeters),
        "M03 authored stratigraphy TOML did not round-trip.");

    // A large conceptual stack increases only profile-level descriptor data.
    // Surface evaluation still returns the same fixed StratigraphySample and
    // does not allocate a 3D or per-texel layer volume.
    StratigraphyProfile manyLayers =
        authored;
    manyLayers.id = {
        .high = 0x4F52424954535452ULL,
        .low = 0x4154000000000100ULL
    };
    manyLayers.name = "Many virtual layers";
    manyLayers.layers.clear();
    manyLayers.layers.reserve(256);

    for (u32 index = 0;
         index < 256;
         ++index)
    {
        manyLayers.layers.push_back({
            .material =
                (index & 1U) == 0U
                    ? reference_rock::Sandstone
                    : reference_rock::Limestone,
            .thicknessMeters = 1.0,
            .transitionBandMeters = 0.0
        });
    }

    const CompiledStratigraphyProfile manyCompiled{
        manyLayers,
        materials
    };
    PlanetSurfacePosition manyQuery =
        anchor;
    const f64 manyTop =
        manyCompiled.DeformedTopRadialOffsetMeters(
            planet,
            manyQuery);
    manyQuery.radialOffsetMeters =
        manyTop - 255.5;
    const StratigraphySample manySample =
        manyCompiled.SampleExposedLayer(
            planet,
            manyQuery);

    ok &= Check(
        manyCompiled.LayerCount() == 256 &&
        manySample.primaryMaterial.IsValid() &&
        sizeof(manySample) ==
            sizeof(StratigraphySample),
        "M03 surface-query storage must remain fixed-size as conceptual layer count grows.");

    bool rejectedUnknownMaterial = false;
    try
    {
        StratigraphyProfile unknown =
            authored;
        unknown.layers[0].material = {
            .high = 0xFFFFFFFFFFFFFFFFULL,
            .low = 0x0000000000000001ULL
        };
        static_cast<void>(
            CompiledStratigraphyProfile{
                std::move(unknown),
                materials
            });
    }
    catch (const std::invalid_argument&)
    {
        rejectedUnknownMaterial = true;
    }

    ok &= Check(
        rejectedUnknownMaterial,
        "M03 runtime profile compilation must reject RockTypeIds absent from M02 authority.");

    bool rejectedInvalidTransition = false;
    try
    {
        StratigraphyProfile invalid =
            authored;
        invalid.layers[0].transitionBandMeters =
            invalid.layers[0].thicknessMeters + 1.0;
        static_cast<void>(
            CompiledStratigraphyProfile{
                std::move(invalid),
                materials
            });
    }
    catch (const std::invalid_argument&)
    {
        rejectedInvalidTransition = true;
    }

    ok &= Check(
        rejectedInvalidTransition,
        "M03 transition bands larger than their layer must be rejected.");

    return ok ? 0 : 1;
}
