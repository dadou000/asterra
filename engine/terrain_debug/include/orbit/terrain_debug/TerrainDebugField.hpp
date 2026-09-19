#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <span>
#include <string_view>

namespace orbit::terrain_debug
{
enum class TerrainDebugField : u8
{
    Uplift = 0,
    BedrockType,
    Strata,
    Regolith,
    Soil,
    Sand,
    Debris,
    Moisture,
    ExposedMaterial,
    Drainage,
    WaterFlux,
    SedimentFlux,
    Wind,
    AeolianFlux,
    ErosionDeposition,
    BiomeWeights,
    FinalBiome,
    ScatterDensity,
    CacheResidency,
    CacheInvalidation,
    PhysicalLod,
    Count
};

inline constexpr u32 kRequiredTerrainDebugFieldCount =
    static_cast<u32>(TerrainDebugField::Count);

enum class TerrainDebugValueClass : u8
{
    Scalar,
    SignedScalar,
    Vector,
    Category,
    Boolean,
    Revision,
    Lod
};

enum class TerrainDebugStage : u8
{
    AuthoredAuthority,
    Geology,
    Stratigraphy,
    MaterialColumn,
    Drainage,
    Hydraulic,
    SedimentExchange,
    WindForcing,
    Aeolian,
    ExposedSurface,
    BiomePlacement,
    BiomeResolution,
    Scatter,
    PersistentCache,
    DependencyInvalidation,
    PhysicalScale
};

struct TerrainDebugFieldDescriptor
{
    TerrainDebugField field{TerrainDebugField::Uplift};
    std::string_view name;
    TerrainDebugValueClass valueClass{
        TerrainDebugValueClass::Scalar};

    // True when the field has physical page support where an edge mismatch
    // is meaningful and should be highlighted by the seam overlay.
    bool seamRelevant{true};

    std::span<const TerrainDebugStage> upstream;
};

[[nodiscard]] std::span<const TerrainDebugFieldDescriptor>
FieldCatalog() noexcept;

[[nodiscard]] const TerrainDebugFieldDescriptor&
Descriptor(TerrainDebugField field);

[[nodiscard]] std::string_view StageName(
    TerrainDebugStage stage) noexcept;

struct TerrainDebugPageStamp
{
    terrain::PhysicalTerrainPageAddress address{};
    u8 physicalLod{0};
    terrain::TerrainGenerationRevisions revisions{};

    bool cacheResident{false};
    u64 invalidationRevision{0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct TerrainDebugSeamProbe
{
    world::TileEdge edge{world::TileEdge::North};

    terrain::PhysicalTerrainPageAddress expectedNeighbor{};

    bool neighborPresent{false};
    bool matchingPhysicalLod{false};
    bool matchingRevisions{false};

    [[nodiscard]] bool IsContinuousCandidate() const noexcept;
};

[[nodiscard]] terrain::PhysicalTerrainPageAddress ExpectedNeighbor(
    const terrain::PhysicalTerrainPageAddress& page,
    world::TileEdge edge) noexcept;

[[nodiscard]] TerrainDebugSeamProbe ProbeSeam(
    const TerrainDebugPageStamp& page,
    world::TileEdge edge,
    const TerrainDebugPageStamp* neighbor) noexcept;

[[nodiscard]] u64 DebugPageFingerprint(
    const TerrainDebugPageStamp& page) noexcept;
} // namespace orbit::terrain_debug
