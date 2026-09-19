#include <orbit/terrain_debug/TerrainDebugField.hpp>

#include <array>
#include <stdexcept>

namespace orbit::terrain_debug
{
namespace
{
using Stage = TerrainDebugStage;

constexpr std::array<Stage, 2> kUplift{
    Stage::AuthoredAuthority,
    Stage::Geology
};
constexpr std::array<Stage, 3> kBedrock{
    Stage::AuthoredAuthority,
    Stage::Geology,
    Stage::MaterialColumn
};
constexpr std::array<Stage, 3> kStrata{
    Stage::AuthoredAuthority,
    Stage::Stratigraphy,
    Stage::MaterialColumn
};
constexpr std::array<Stage, 3> kLooseMaterial{
    Stage::Geology,
    Stage::MaterialColumn,
    Stage::ExposedSurface
};
constexpr std::array<Stage, 4> kMoisture{
    Stage::MaterialColumn,
    Stage::Hydraulic,
    Stage::ExposedSurface,
    Stage::BiomePlacement
};
constexpr std::array<Stage, 3> kExposed{
    Stage::MaterialColumn,
    Stage::Stratigraphy,
    Stage::ExposedSurface
};
constexpr std::array<Stage, 4> kDrainage{
    Stage::AuthoredAuthority,
    Stage::MaterialColumn,
    Stage::Drainage,
    Stage::Hydraulic
};
constexpr std::array<Stage, 4> kWaterFlux{
    Stage::MaterialColumn,
    Stage::Drainage,
    Stage::Hydraulic,
    Stage::SedimentExchange
};
constexpr std::array<Stage, 4> kSedimentFlux{
    Stage::MaterialColumn,
    Stage::Hydraulic,
    Stage::Aeolian,
    Stage::SedimentExchange
};
constexpr std::array<Stage, 2> kWind{
    Stage::AuthoredAuthority,
    Stage::WindForcing
};
constexpr std::array<Stage, 4> kAeolian{
    Stage::WindForcing,
    Stage::MaterialColumn,
    Stage::Aeolian,
    Stage::SedimentExchange
};
constexpr std::array<Stage, 5> kErosionDeposition{
    Stage::Geology,
    Stage::MaterialColumn,
    Stage::Hydraulic,
    Stage::Aeolian,
    Stage::SedimentExchange
};
constexpr std::array<Stage, 4> kBiomeWeights{
    Stage::ExposedSurface,
    Stage::AuthoredAuthority,
    Stage::BiomePlacement,
    Stage::BiomeResolution
};
constexpr std::array<Stage, 5> kFinalBiome{
    Stage::ExposedSurface,
    Stage::AuthoredAuthority,
    Stage::BiomePlacement,
    Stage::BiomeResolution,
    Stage::Scatter
};
constexpr std::array<Stage, 5> kScatter{
    Stage::MaterialColumn,
    Stage::ExposedSurface,
    Stage::BiomePlacement,
    Stage::BiomeResolution,
    Stage::Scatter
};
constexpr std::array<Stage, 2> kCache{
    Stage::DependencyInvalidation,
    Stage::PersistentCache
};
constexpr std::array<Stage, 2> kInvalidation{
    Stage::AuthoredAuthority,
    Stage::DependencyInvalidation
};
constexpr std::array<Stage, 1> kPhysicalLod{
    Stage::PhysicalScale
};

const std::array<TerrainDebugFieldDescriptor, kRequiredTerrainDebugFieldCount>
kCatalog{{
    {TerrainDebugField::Uplift, "Uplift", TerrainDebugValueClass::SignedScalar, true, kUplift},
    {TerrainDebugField::BedrockType, "Bedrock Type", TerrainDebugValueClass::Category, true, kBedrock},
    {TerrainDebugField::Strata, "Strata", TerrainDebugValueClass::Category, true, kStrata},
    {TerrainDebugField::Regolith, "Regolith", TerrainDebugValueClass::Scalar, true, kLooseMaterial},
    {TerrainDebugField::Soil, "Soil", TerrainDebugValueClass::Scalar, true, kLooseMaterial},
    {TerrainDebugField::Sand, "Sand", TerrainDebugValueClass::Scalar, true, kLooseMaterial},
    {TerrainDebugField::Debris, "Debris", TerrainDebugValueClass::Scalar, true, kLooseMaterial},
    {TerrainDebugField::Moisture, "Moisture", TerrainDebugValueClass::Scalar, true, kMoisture},
    {TerrainDebugField::ExposedMaterial, "Exposed Material", TerrainDebugValueClass::Category, true, kExposed},
    {TerrainDebugField::Drainage, "Drainage", TerrainDebugValueClass::Vector, true, kDrainage},
    {TerrainDebugField::WaterFlux, "Water Flux", TerrainDebugValueClass::Vector, true, kWaterFlux},
    {TerrainDebugField::SedimentFlux, "Sediment Flux", TerrainDebugValueClass::Vector, true, kSedimentFlux},
    {TerrainDebugField::Wind, "Wind", TerrainDebugValueClass::Vector, true, kWind},
    {TerrainDebugField::AeolianFlux, "Aeolian Flux", TerrainDebugValueClass::Vector, true, kAeolian},
    {TerrainDebugField::ErosionDeposition, "Erosion / Deposition", TerrainDebugValueClass::SignedScalar, true, kErosionDeposition},
    {TerrainDebugField::BiomeWeights, "Biome Weights", TerrainDebugValueClass::Scalar, true, kBiomeWeights},
    {TerrainDebugField::FinalBiome, "Final Biome", TerrainDebugValueClass::Category, true, kFinalBiome},
    {TerrainDebugField::ScatterDensity, "Scatter Density", TerrainDebugValueClass::Scalar, true, kScatter},
    {TerrainDebugField::CacheResidency, "Cache Residency", TerrainDebugValueClass::Boolean, true, kCache},
    {TerrainDebugField::CacheInvalidation, "Cache Invalidation", TerrainDebugValueClass::Revision, true, kInvalidation},
    {TerrainDebugField::PhysicalLod, "Physical LOD", TerrainDebugValueClass::Lod, true, kPhysicalLod}
}};

static_assert(
    kCatalog.size() ==
    static_cast<std::size_t>(TerrainDebugField::Count));
} // namespace

std::span<const TerrainDebugFieldDescriptor>
FieldCatalog() noexcept
{
    return kCatalog;
}

const TerrainDebugFieldDescriptor&
Descriptor(const TerrainDebugField field)
{
    const auto index =
        static_cast<std::size_t>(field);

    if (index >= kCatalog.size())
    {
        throw std::out_of_range(
            "Unknown M29 terrain debug field.");
    }

    return kCatalog[index];
}

std::string_view StageName(
    const TerrainDebugStage stage) noexcept
{
    switch (stage)
    {
    case Stage::AuthoredAuthority:
        return "Authored Authority";
    case Stage::Geology:
        return "Geology / Uplift";
    case Stage::Stratigraphy:
        return "Virtual Stratigraphy";
    case Stage::MaterialColumn:
        return "Physical Material Column";
    case Stage::Drainage:
        return "Drainage";
    case Stage::Hydraulic:
        return "Hydraulic / Water";
    case Stage::SedimentExchange:
        return "Unified Sediment";
    case Stage::WindForcing:
        return "Wind Forcing";
    case Stage::Aeolian:
        return "Aeolian";
    case Stage::ExposedSurface:
        return "Exposed Surface";
    case Stage::BiomePlacement:
        return "Biome Placement";
    case Stage::BiomeResolution:
        return "Biome Resolution";
    case Stage::Scatter:
        return "Deterministic Scatter";
    case Stage::PersistentCache:
        return "Persistent GPU Cache";
    case Stage::DependencyInvalidation:
        return "Dependency Invalidation";
    case Stage::PhysicalScale:
        return "Physical Scale Hierarchy";
    }

    return "Unknown";
}

bool TerrainDebugPageStamp::IsValid() const noexcept
{
    return address.planet.IsValid();
}

bool TerrainDebugSeamProbe::IsContinuousCandidate() const noexcept
{
    return
        neighborPresent &&
        matchingPhysicalLod &&
        matchingRevisions;
}

terrain::PhysicalTerrainPageAddress ExpectedNeighbor(
    const terrain::PhysicalTerrainPageAddress& page,
    const world::TileEdge edge) noexcept
{
    return {
        .planet = page.planet,
        .tile =
            world::NeighborAcrossTileEdge(
                page.tile,
                edge).tile
    };
}

TerrainDebugSeamProbe ProbeSeam(
    const TerrainDebugPageStamp& page,
    const world::TileEdge edge,
    const TerrainDebugPageStamp* neighbor) noexcept
{
    TerrainDebugSeamProbe result{
        .edge = edge,
        .expectedNeighbor =
            ExpectedNeighbor(
                page.address,
                edge)
    };

    if (!page.IsValid() ||
        neighbor == nullptr ||
        !neighbor->IsValid() ||
        neighbor->address !=
            result.expectedNeighbor)
    {
        return result;
    }

    result.neighborPresent = true;
    result.matchingPhysicalLod =
        neighbor->physicalLod ==
        page.physicalLod;
    result.matchingRevisions =
        neighbor->revisions ==
        page.revisions;

    return result;
}

u64 DebugPageFingerprint(
    const TerrainDebugPageStamp& page) noexcept
{
    u64 value =
        terrain::StableCombine64(
            0x4D32394442475047ULL,
            page.address.planet.high);

    value =
        terrain::StableCombine64(
            value,
            page.address.planet.low);
    value =
        terrain::StableCombine64(
            value,
            static_cast<u64>(
                page.address.tile.face));
    value =
        terrain::StableCombine64(
            value,
            page.address.tile.level);
    value =
        terrain::StableCombine64(
            value,
            page.address.tile.x);
    value =
        terrain::StableCombine64(
            value,
            page.address.tile.y);
    value =
        terrain::StableCombine64(
            value,
            page.physicalLod);
    value =
        terrain::StableCombine64(
            value,
            terrain::RevisionFingerprint(
                page.revisions));
    value =
        terrain::StableCombine64(
            value,
            page.cacheResident ? 1U : 0U);
    value =
        terrain::StableCombine64(
            value,
            page.invalidationRevision);

    return value;
}
} // namespace orbit::terrain_debug
