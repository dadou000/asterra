#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/world/Planet.hpp>

#include <cstddef>
#include <span>

namespace orbit::terrain
{
// M00 authority contract. Authored intent, canonical physical state and
// derived/view state stay separate so editor, renderer and cache code cannot
// silently become terrain simulation authorities.
enum class TerrainAuthorityDomain : u8
{
    DocumentAuthoring,
    TerrainPhysical,
    WaterPhysical,
    BiomePolicy,
    DerivedCache,
    ViewInterest
};

[[nodiscard]] constexpr bool OwnsCanonicalTerrain(
    const TerrainAuthorityDomain domain) noexcept
{
    return domain == TerrainAuthorityDomain::TerrainPhysical;
}

[[nodiscard]] constexpr bool OwnsCanonicalWater(
    const TerrainAuthorityDomain domain) noexcept
{
    return domain == TerrainAuthorityDomain::WaterPhysical;
}

[[nodiscard]] constexpr bool IsDerivedOnly(
    const TerrainAuthorityDomain domain) noexcept
{
    return domain == TerrainAuthorityDomain::DerivedCache ||
           domain == TerrainAuthorityDomain::ViewInterest;
}

// Residency describes where a derived/canonical representation is currently
// materialized. It is deliberately not an authority or identity field.
enum class TerrainResidency : u8
{
    Absent,
    Cpu,
    Gpu,
    CpuAndGpu
};

// Every domain that can change canonical generated terrain has an explicit,
// monotonic revision. View/camera/render revisions are intentionally absent.
struct TerrainGenerationRevisions
{
    u64 geology{0};
    u64 climate{0};
    u64 authoring{0};
    u64 biome{0};
    u64 water{0};
    u64 processes{0};

    [[nodiscard]] constexpr bool operator==(
        const TerrainGenerationRevisions&) const noexcept = default;
};

// Stable planet-space address. Render clipmap rings, GPU slots and frame state
// are not represented here, so moving a camera cannot change physical page
// identity by construction.
struct PhysicalTerrainPageAddress
{
    world::PlanetId planet{};
    world::PlanetTileId tile{};

    [[nodiscard]] constexpr bool operator==(
        const PhysicalTerrainPageAddress&) const noexcept = default;
};

// Complete generated-page identity. Resolution and generation revisions
// invalidate cached output without changing the stable page address.
struct PhysicalTerrainPageKey
{
    PhysicalTerrainPageAddress address{};
    u32 resolution{65};
    TerrainGenerationRevisions revisions{};

    [[nodiscard]] constexpr bool operator==(
        const PhysicalTerrainPageKey&) const noexcept = default;
};

// Fixed, platform-independent mixing used only for persisted/cache identity and
// procedural seed derivation. std::hash is deliberately excluded because its
// representation is not an Orbit persistence contract.
[[nodiscard]] constexpr u64 StableMix64(u64 value) noexcept
{
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) *
            0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) *
            0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] constexpr u64 StableCombine64(
    const u64 seed,
    const u64 value) noexcept
{
    return StableMix64(
        seed ^ StableMix64(value));
}

[[nodiscard]] constexpr u64 RevisionFingerprint(
    const TerrainGenerationRevisions& revisions) noexcept
{
    u64 value = 0x4F52424954524556ULL; // "ORBITREV"
    value = StableCombine64(value, revisions.geology);
    value = StableCombine64(value, revisions.climate);
    value = StableCombine64(value, revisions.authoring);
    value = StableCombine64(value, revisions.biome);
    value = StableCombine64(value, revisions.water);
    value = StableCombine64(value, revisions.processes);
    return value;
}

[[nodiscard]] constexpr u64 PhysicalPageFingerprint(
    const PhysicalTerrainPageKey& key) noexcept
{
    u64 value = 0x4F52424954504745ULL; // "ORBITPGE"
    value = StableCombine64(value, key.address.planet.high);
    value = StableCombine64(value, key.address.planet.low);
    value = StableCombine64(
        value,
        static_cast<u64>(key.address.tile.face));
    value = StableCombine64(value, key.address.tile.level);
    value = StableCombine64(value, key.address.tile.x);
    value = StableCombine64(value, key.address.tile.y);
    value = StableCombine64(value, key.resolution);
    value = StableCombine64(
        value,
        RevisionFingerprint(key.revisions));
    return value;
}

// Numeric values are persisted domain separators. Append new domains; never
// renumber existing entries once project data depends on them.
enum class TerrainSeedDomain : u64
{
    BaseRelief = 0x01ULL,
    Geology = 0x02ULL,
    Climate = 0x03ULL,
    Hydrology = 0x04ULL,
    HydraulicErosion = 0x05ULL,
    AeolianErosion = 0x06ULL,
    ThermalErosion = 0x07ULL,
    Sediment = 0x08ULL,
    Biome = 0x09ULL,
    AuthoredFeature = 0x0AULL
};

// Revisions and page resolution are intentionally absent: changing parameters
// should invalidate/rebuild a page without randomly re-seeding its stable
// spatial features. Optional object halves let authored StrongIds participate
// without coupling terrain to a particular document type.
[[nodiscard]] constexpr u64 DeriveTerrainSeed(
    const u64 planetRootSeed,
    const TerrainSeedDomain domain,
    const PhysicalTerrainPageAddress& address,
    const u64 stableObjectHigh = 0,
    const u64 stableObjectLow = 0) noexcept
{
    u64 value = StableCombine64(
        0x4F52424954534545ULL, // "ORBITSEE"
        planetRootSeed);
    value = StableCombine64(
        value,
        static_cast<u64>(domain));
    value = StableCombine64(value, address.planet.high);
    value = StableCombine64(value, address.planet.low);
    value = StableCombine64(
        value,
        static_cast<u64>(address.tile.face));
    value = StableCombine64(value, address.tile.level);
    value = StableCombine64(value, address.tile.x);
    value = StableCombine64(value, address.tile.y);
    value = StableCombine64(value, stableObjectHigh);
    value = StableCombine64(value, stableObjectLow);
    return value;
}

// Immutable field view exposed to BiomeService and other policy/evaluation
// systems. Those systems may derive weights/coefficients from physical state,
// but they cannot mutate canonical material buffers through this interface.
struct TerrainScalarFieldView
{
    std::span<const f32> samples{};
    u32 width{0};
    u32 height{0};
    u32 rowStride{0};

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        if (width == 0 || height == 0 || rowStride < width)
        {
            return false;
        }

        const std::size_t required =
            static_cast<std::size_t>(rowStride) *
            static_cast<std::size_t>(height - 1U) +
            static_cast<std::size_t>(width);

        return samples.size() >= required;
    }

    [[nodiscard]] constexpr f32 At(
        const u32 x,
        const u32 y) const noexcept
    {
        return samples[
            static_cast<std::size_t>(y) *
                static_cast<std::size_t>(rowStride) +
            static_cast<std::size_t>(x)];
    }
};

struct TerrainSurfaceView
{
    TerrainScalarFieldView bedrockHeightMeters{};
    TerrainScalarFieldView soilDepthMeters{};
    TerrainScalarFieldView sandDepthMeters{};
    TerrainScalarFieldView debrisDepthMeters{};
    TerrainScalarFieldView moisture{};
    TerrainScalarFieldView hardness{};
};
} // namespace orbit::terrain
