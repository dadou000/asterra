#include <orbit/terrain/TerrainContracts.hpp>

#include <array>
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
