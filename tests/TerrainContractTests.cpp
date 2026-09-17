#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain/TerrainPosition.hpp>
#include <orbit/terrain/TerrainSource.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <type_traits>

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
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::terrain;

    static_assert(
        std::is_const_v<
            typename decltype(
                TerrainScalarFieldView{}.samples)::element_type>,
        "Biome-facing terrain fields must remain immutable.");

    constexpr world::PlanetId planetId{
        .high = 0x0123456789ABCDEFULL,
        .low = 0xFEDCBA9876543210ULL
    };

    constexpr PhysicalTerrainPageAddress address{
        .planet = planetId,
        .tile = {
            .face = world::CubeFace::PositiveZ,
            .level = 12,
            .x = 137,
            .y = 911
        }
    };

    constexpr TerrainGenerationRevisions revisions{
        .geology = 7,
        .climate = 11,
        .authoring = 13,
        .biome = 17,
        .water = 19,
        .processes = 23
    };

    static_assert(
        RevisionForDomain(
            revisions,
            TerrainRevisionDomain::Geology) == 7 &&
        RevisionForDomain(
            revisions,
            TerrainRevisionDomain::Climate) == 11 &&
        RevisionForDomain(
            revisions,
            TerrainRevisionDomain::Authoring) == 13 &&
        RevisionForDomain(
            revisions,
            TerrainRevisionDomain::Biome) == 17 &&
        RevisionForDomain(
            revisions,
            TerrainRevisionDomain::Water) == 19 &&
        RevisionForDomain(
            revisions,
            TerrainRevisionDomain::Processes) == 23,
        "Procedural graph source domains must map to the correct revisions.");

    constexpr PhysicalTerrainPageKey key{
        .address = address,
        .resolution = 129,
        .revisions = revisions
    };

    constexpr u64 fingerprint =
        PhysicalPageFingerprint(key);

    static_assert(
        fingerprint == PhysicalPageFingerprint(key),
        "Physical page fingerprints must be deterministic.");

    auto waterChanged = key;
    ++waterChanged.revisions.water;

    auto authoringChanged = key;
    ++authoringChanged.revisions.authoring;

    bool ok = true;
    ok &= Check(
        fingerprint !=
            PhysicalPageFingerprint(waterChanged),
        "WaterService revision must invalidate physical terrain pages.");
    ok &= Check(
        fingerprint !=
            PhysicalPageFingerprint(authoringChanged),
        "Authoring revision must invalidate physical terrain pages.");

    // Camera/view state is deliberately not accepted by either page identity
    // or seed APIs. Changing these values therefore cannot alter the key.
    [[maybe_unused]] f64 cameraAltitudeMeters = 100.0;
    [[maybe_unused]] u64 frameNumber = 1;
    const u64 beforeViewMove =
        PhysicalPageFingerprint(key);
    cameraAltitudeMeters = 150'000.0;
    frameNumber = 9'000;
    const u64 afterViewMove =
        PhysicalPageFingerprint(key);

    ok &= Check(
        beforeViewMove == afterViewMove,
        "View/camera state must not affect physical page identity.");

    constexpr u64 rootSeed = 0xA57E22A57E22AULL;
    constexpr u64 hydraulicSeed =
        DeriveTerrainSeed(
            rootSeed,
            TerrainSeedDomain::HydraulicErosion,
            address);
    constexpr u64 hydraulicSeedAgain =
        DeriveTerrainSeed(
            rootSeed,
            TerrainSeedDomain::HydraulicErosion,
            address);
    constexpr u64 aeolianSeed =
        DeriveTerrainSeed(
            rootSeed,
            TerrainSeedDomain::AeolianErosion,
            address);

    static_assert(
        hydraulicSeed == hydraulicSeedAgain,
        "Terrain seed derivation must be deterministic.");
    static_assert(
        hydraulicSeed != aeolianSeed,
        "Procedural domains must use distinct deterministic seeds.");

    auto otherAddress = address;
    ++otherAddress.tile.x;

    ok &= Check(
        hydraulicSeed !=
            DeriveTerrainSeed(
                rootSeed,
                TerrainSeedDomain::HydraulicErosion,
                otherAddress),
        "Physical page address must participate in deterministic seeds.");

    ok &= Check(
        hydraulicSeed !=
            DeriveTerrainSeed(
                rootSeed,
                TerrainSeedDomain::HydraulicErosion,
                address,
                0x1234ULL,
                0x5678ULL),
        "Stable authored object IDs must be able to domain-separate seeds.");

    ok &= Check(
        OwnsCanonicalTerrain(
            TerrainAuthorityDomain::TerrainPhysical),
        "TerrainPhysical must own canonical terrain state.");
    ok &= Check(
        !OwnsCanonicalTerrain(
            TerrainAuthorityDomain::BiomePolicy),
        "BiomePolicy must not own canonical terrain state.");
    ok &= Check(
        OwnsCanonicalWater(
            TerrainAuthorityDomain::WaterPhysical),
        "WaterPhysical must own canonical water state.");
    ok &= Check(
        IsDerivedOnly(
            TerrainAuthorityDomain::DerivedCache) &&
            IsDerivedOnly(
                TerrainAuthorityDomain::ViewInterest),
        "Cache and view state must remain derived-only.");

    // M01: the same physical direction expressed on two adjacent cube faces
    // canonicalizes to one direction and therefore one physical tile.
    const PlanetSurfacePosition positiveXEdge =
        SurfacePositionFromCube(
            planetId,
            {
                .face = world::CubeFace::PositiveX,
                .uv = {-1.0, 0.0}
            });

    const PlanetSurfacePosition positiveZEdge =
        SurfacePositionFromCube(
            planetId,
            {
                .face = world::CubeFace::PositiveZ,
                .uv = {1.0, 0.0}
            });

    ok &= Check(
        math::Dot(
            positiveXEdge.unitDirection,
            positiveZEdge.unitDirection) >
            1.0 - 1.0e-12,
        "Equivalent cube-face boundary coordinates must canonicalize to the same surface direction.");

    ok &= Check(
        PhysicalTileForPosition(
            positiveXEdge,
            10) ==
            PhysicalTileForPosition(
                positiveZEdge,
                10),
        "Equivalent face-boundary samples must resolve to one physical tile identity.");

    const world::SurfaceFrame edgeFrame =
        SurfaceTangentFrame(positiveXEdge);

    ok &= Check(
        std::abs(
            math::Dot(
                edgeFrame.east,
                edgeFrame.up)) < 1.0e-12 &&
        std::abs(
            math::Dot(
                edgeFrame.north,
                edgeFrame.up)) < 1.0e-12,
        "Canonical surface tangent frame must remain tangent at face boundaries.");

    const TerrainSampleFootprint footprint{
        .diameterMeters = 64.0
    };

    ok &= Check(
        footprint.IsValid() &&
            footprint.MinimumResolvedWavelengthMeters() ==
                128.0,
        "LOD sampling footprint must be represented in physical meters.");

    const TerrainQuery query{
        .unitDirection = {2.0, 0.0, 0.0},
        .footprintMeters = 64.0,
        .planet = planetId,
        .radialOffsetMeters = 25.0
    };

    const PlanetSurfacePosition queryPosition =
        query.SurfacePosition();

    ok &= Check(
        queryPosition.planet == planetId &&
            std::abs(
                math::Length(
                    queryPosition.unitDirection) -
                1.0) < 1.0e-12 &&
            queryPosition.radialOffsetMeters == 25.0 &&
            query.Footprint().diameterMeters == 64.0,
        "TerrainQuery must transport canonical planet position and physical footprint.");

    const TerrainQuery rebuiltQuery =
        MakeTerrainQuery(
            queryPosition,
            footprint);

    ok &= Check(
        rebuiltQuery.planet == planetId &&
            rebuiltQuery.unitDirection ==
                queryPosition.unitDirection &&
            rebuiltQuery.radialOffsetMeters == 25.0 &&
            rebuiltQuery.footprintMeters == 64.0,
        "Canonical position and footprint must reconstruct a stable terrain query.");

    const world::PlanetDefinition testPlanet{
        .radiusMeters = 6'000'000.0,
        .id = planetId
    };

    const PlanetSurfacePosition offsetPosition =
        OffsetSurfacePosition(
            testPlanet,
            positiveXEdge,
            edgeFrame,
            {1'250.0, -775.0});

    const math::Double2 recoveredOffset =
        SurfaceOffsetBetweenPositions(
            testPlanet,
            positiveXEdge,
            edgeFrame,
            offsetPosition);

    // The inverse uses acos on a ~0.014-degree angular separation. A
    // 0.1-millimetre bound is far tighter than any terrain sample while
    // remaining stable under expected double-precision conditioning.
    constexpr f64 geodesicToleranceMeters = 1.0e-4;

    ok &= Check(
        std::abs(recoveredOffset.x - 1'250.0) <
                geodesicToleranceMeters &&
            std::abs(recoveredOffset.y + 775.0) <
                geodesicToleranceMeters,
        "Canonical tangent offset conversion must round-trip in physical meters.");

    const PlanetSurfacePosition otherPlanetPosition{
        .planet = {
            .high = 0xAAULL,
            .low = 0xBBULL
        },
        .unitDirection = positiveXEdge.unitDirection
    };

    const math::Double2 crossPlanetOffset =
        SurfaceOffsetBetweenPositions(
            testPlanet,
            positiveXEdge,
            edgeFrame,
            otherPlanetPosition);

    ok &= Check(
        std::isinf(crossPlanetOffset.x) &&
            std::isinf(crossPlanetOffset.y),
        "Surface offsets must reject cross-planet coordinate aliasing.");

    std::array<f32, 4> bedrock{
        10.0F,
        11.0F,
        12.0F,
        13.0F
    };
    const TerrainScalarFieldView bedrockView{
        .samples = bedrock,
        .width = 2,
        .height = 2,
        .rowStride = 2
    };

    ok &= Check(
        bedrockView.IsValid() &&
            bedrockView.At(1, 1) == 13.0F,
        "Immutable physical field views must preserve layout and sampling.");

    return ok ? 0 : 1;
}
