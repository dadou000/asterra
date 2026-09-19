#include <orbit/terrain_debug/TerrainDebugPageData.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace orbit::terrain_debug
{
namespace
{
[[nodiscard]] u32 RockCategory(
    const terrain_geology::RockTypeId id) noexcept
{
    const u64 mixed =
        terrain::StableCombine64(
            id.high,
            id.low);
    return static_cast<u32>(
        mixed ^
        (mixed >> 32U));
}

[[nodiscard]] u32 ExposedCategory(
    const terrain_material_column::ExposedSurfaceKind kind) noexcept
{
    return static_cast<u32>(kind);
}

[[nodiscard]] u32 StableCategory(
    const u64 high,
    const u64 low) noexcept
{
    const u64 mixed =
        terrain::StableCombine64(high, low);
    return static_cast<u32>(mixed ^ (mixed >> 32U));
}

[[nodiscard]] u32 StratigraphyCategory(
    const terrain_geology::StratigraphySample& sample) noexcept
{
    u64 value =
        terrain::StableCombine64(
            sample.primaryMaterial.high,
            sample.primaryMaterial.low);
    value =
        terrain::StableCombine64(
            value,
            sample.layerIndex);
    value =
        terrain::StableCombine64(
            value,
            sample.basement ? 1U : 0U);
    return static_cast<u32>(value ^ (value >> 32U));
}
} // namespace

TerrainDebugPageData::TerrainDebugPageData(
    TerrainDebugPageStamp stamp,
    const u32 width,
    const u32 height)
    : stamp_(stamp),
      width_(width),
      height_(height)
{
    if (!stamp_.IsValid())
    {
        throw std::invalid_argument(
            "Terrain debug page data requires a valid physical page stamp.");
    }

    if (width_ == 0U || height_ == 0U)
    {
        throw std::invalid_argument(
            "Terrain debug page data dimensions must be non-zero.");
    }

    CapturePageProvenance();
}

const TerrainDebugPageStamp&
TerrainDebugPageData::Stamp() const noexcept
{
    return stamp_;
}

u32 TerrainDebugPageData::Width() const noexcept
{
    return width_;
}

u32 TerrainDebugPageData::Height() const noexcept
{
    return height_;
}

void TerrainDebugPageData::CaptureMaterialColumn(
    const terrain_material_column::MaterialColumnPage& page)
{
    if (page.Resolution() != width_ ||
        page.Resolution() != height_)
    {
        throw std::invalid_argument(
            "M08 material-column resolution does not match the M29 debug page.");
    }

    const auto cells = page.Cells();
    RequireTexelCount(cells.size(), "material column");

    auto& bedrock =
        fields_[Index(TerrainDebugField::BedrockType)].category;
    auto& regolith =
        fields_[Index(TerrainDebugField::Regolith)].scalar;
    auto& soil =
        fields_[Index(TerrainDebugField::Soil)].scalar;
    auto& sand =
        fields_[Index(TerrainDebugField::Sand)].scalar;
    auto& debris =
        fields_[Index(TerrainDebugField::Debris)].scalar;
    auto& moisture =
        fields_[Index(TerrainDebugField::Moisture)].scalar;
    auto& exposed =
        fields_[Index(TerrainDebugField::ExposedMaterial)].category;

    bedrock.resize(cells.size());
    regolith.resize(cells.size());
    soil.resize(cells.size());
    sand.resize(cells.size());
    debris.resize(cells.size());
    moisture.resize(cells.size());
    exposed.resize(cells.size());

    for (std::size_t i = 0; i < cells.size(); ++i)
    {
        const auto& cell = cells[i];

        bedrock[i] =
            RockCategory(cell.bedrockMaterial);
        regolith[i] = cell.regolithMeters;
        soil[i] = cell.soilMeters;
        sand[i] = cell.sandMeters;
        debris[i] = cell.debrisMeters;
        moisture[i] = cell.moisture;
        exposed[i] =
            ExposedCategory(
                cell.ExposedSurface());
    }
}

void TerrainDebugPageData::CaptureDrainage(
    const terrain_hydrology::DrainagePage& page)
{
    if (page.Resolution() != width_ ||
        page.Resolution() != height_)
    {
        throw std::invalid_argument(
            "M09 drainage resolution does not match the M29 debug page.");
    }

    auto& drainage =
        fields_[Index(TerrainDebugField::Drainage)].vector;
    drainage.resize(
        static_cast<std::size_t>(
            TexelCount()));

    for (u32 y = 0; y < height_; ++y)
    {
        for (u32 x = 0; x < width_; ++x)
        {
            const auto& cell =
                page.At(x, y);
            const auto offset =
                static_cast<std::size_t>(y) *
                    width_ +
                x;

            // Direction is canonical M09 D8 routing; magnitude is discharge.
            // The debug vector therefore exposes both where flow leaves the
            // cell and how much routed water it represents.
            const f32 magnitude =
                static_cast<f32>(
                    std::max(
                        cell.dischargeCubicMetersPerSecond,
                        0.0));

            drainage[offset] = {
                .x =
                    static_cast<f32>(
                        cell.flow.dx) *
                    magnitude,
                .y =
                    static_cast<f32>(
                        cell.flow.dy) *
                    magnitude
            };
        }
    }
}

void TerrainDebugPageData::CaptureHydraulic(
    const terrain_erosion::HydraulicErosionResult& result)
{
    RequireTexelCount(
        result.cells.size(),
        "hydraulic result");

    if (result.material.Resolution() != width_ ||
        result.material.Resolution() != height_)
    {
        throw std::invalid_argument(
            "M11 hydraulic material resolution does not match the M29 debug page.");
    }

    auto& waterFlux =
        fields_[Index(TerrainDebugField::WaterFlux)].vector;
    auto& erosionDeposition =
        fields_[Index(TerrainDebugField::ErosionDeposition)].scalar;

    waterFlux.resize(result.cells.size());
    erosionDeposition.resize(result.cells.size());

    for (std::size_t i = 0;
         i < result.cells.size();
         ++i)
    {
        const auto& cell = result.cells[i];

        waterFlux[i] = {
            .x = static_cast<f32>(
                cell.fluxEastCubicMetersPerSecond -
                cell.fluxWestCubicMetersPerSecond),
            .y = static_cast<f32>(
                cell.fluxNorthCubicMetersPerSecond -
                cell.fluxSouthCubicMetersPerSecond)
        };

        // Positive means net deposition, negative means net erosion.
        erosionDeposition[i] =
            static_cast<f32>(
                cell.cumulativeDepositedDepthMeters -
                cell.cumulativeErodedDepthMeters);
    }
}

void TerrainDebugPageData::CaptureSedimentExchange(
    const terrain_erosion::SedimentExchangePage& page)
{
    if (page.Resolution() != width_ ||
        page.Resolution() != height_)
    {
        throw std::invalid_argument(
            "M14 sediment-exchange resolution does not match the M29 debug page.");
    }

    auto& flux =
        fields_[Index(TerrainDebugField::SedimentFlux)].vector;
    flux.resize(
        static_cast<std::size_t>(
            TexelCount()));

    const f64 inverseArea =
        1.0 /
        std::max(
            page.SpacingMeters() *
                page.SpacingMeters(),
            1.0e-12);

    for (u32 y = 0; y < height_; ++y)
    {
        for (u32 x = 0; x < width_; ++x)
        {
            const auto total =
                page.TransportAt(x, y).
                    Total();

            const auto index =
                static_cast<std::size_t>(y) *
                    width_ +
                x;

            flux[index] = {
                .x = static_cast<f32>(
                    total.eastKg *
                    inverseArea),
                .y = static_cast<f32>(
                    total.northKg *
                    inverseArea)
            };
        }
    }
}

void TerrainDebugPageData::CaptureMacroGeology(
    const std::span<const terrain_macro_geology::MacroGeologySample> samples)
{
    RequireTexelCount(samples.size(), "macro geology");

    auto& uplift =
        fields_[Index(TerrainDebugField::Uplift)].scalar;
    uplift.resize(samples.size());

    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        uplift[i] =
            static_cast<f32>(samples[i].upliftMeters);
    }
}

void TerrainDebugPageData::CaptureStratigraphy(
    const std::span<const terrain_geology::StratigraphySample> samples)
{
    RequireTexelCount(samples.size(), "stratigraphy");

    auto& strata =
        fields_[Index(TerrainDebugField::Strata)].category;
    strata.resize(samples.size());

    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        strata[i] =
            StratigraphyCategory(samples[i]);
    }
}

void TerrainDebugPageData::CaptureAeolian(
    const std::span<const terrain_erosion::AeolianCellForcing> forcing,
    const terrain_erosion::AeolianErosionResult& result)
{
    RequireTexelCount(forcing.size(), "aeolian forcing");
    RequireTexelCount(result.cells.size(), "aeolian result");

    auto& wind =
        fields_[Index(TerrainDebugField::Wind)].vector;
    auto& flux =
        fields_[Index(TerrainDebugField::AeolianFlux)].vector;
    wind.resize(forcing.size());
    flux.assign(forcing.size(), {});

    const auto* sediment =
        result.sedimentExchange.has_value()
            ? &*result.sedimentExchange
            : nullptr;

    if (sediment != nullptr &&
        (sediment->Resolution() != width_ ||
         sediment->Resolution() != height_))
    {
        throw std::invalid_argument(
            "M13 sediment-exchange resolution does not match the M29 debug page.");
    }

    const f64 cellArea =
        sediment != nullptr
            ? sediment->SpacingMeters() *
                  sediment->SpacingMeters()
            : 1.0;

    for (u32 y = 0; y < height_; ++y)
    {
        for (u32 x = 0; x < width_; ++x)
        {
            const std::size_t i =
                static_cast<std::size_t>(y) *
                    width_ +
                x;
            const f32 east =
                forcing[i].windEastMetersPerSecond;
            const f32 north =
                forcing[i].windNorthMetersPerSecond;

            wind[i] = {
                .x = east,
                .y = north
            };

            if (sediment == nullptr)
            {
                continue;
            }

            const f64 speed =
                std::hypot(
                    static_cast<f64>(east),
                    static_cast<f64>(north));
            if (speed <= 0.0)
            {
                continue;
            }

            const auto& mobile =
                sediment->At(x, y);
            const f64 transportedKg =
                mobile.airborne.TotalKg() +
                mobile.surfaceMobile.TotalKg();
            const f64 density =
                transportedKg /
                std::max(cellArea, 1.0e-12);

            flux[i] = {
                .x = static_cast<f32>(
                    static_cast<f64>(east) /
                    speed * density),
                .y = static_cast<f32>(
                    static_cast<f64>(north) /
                    speed * density)
            };
        }
    }
}

void TerrainDebugPageData::CaptureBiomeResolution(
    const std::span<const f32> dominantWeights,
    const std::span<const terrain_biome::BiomeId> dominantBiomes)
{
    RequireTexelCount(dominantWeights.size(), "biome weights");
    RequireTexelCount(dominantBiomes.size(), "final biome");

    auto& weights =
        fields_[Index(TerrainDebugField::BiomeWeights)].scalar;
    auto& finalBiome =
        fields_[Index(TerrainDebugField::FinalBiome)].category;

    weights.assign(
        dominantWeights.begin(),
        dominantWeights.end());
    finalBiome.resize(dominantBiomes.size());

    for (std::size_t i = 0; i < dominantBiomes.size(); ++i)
    {
        finalBiome[i] =
            StableCategory(
                dominantBiomes[i].high,
                dominantBiomes[i].low);
    }
}

void TerrainDebugPageData::CaptureScatterDensity(
    const terrain_scatter::ScatterPageRequest& request,
    const std::span<const terrain_scatter::DerivedScatterInstance> instances)
{
    if (request.gridResolution != width_ ||
        request.gridResolution != height_ ||
        !request.IsValid())
    {
        throw std::invalid_argument(
            "M22 scatter request does not match the M29 debug page.");
    }

    auto& density =
        fields_[Index(TerrainDebugField::ScatterDensity)].scalar;
    density.assign(
        static_cast<std::size_t>(TexelCount()),
        0.0F);

    const f32 cellArea =
        request.cellSizeMeters *
        request.cellSizeMeters;

    for (const auto& instance : instances)
    {
        if (instance.cellX >= width_ ||
            instance.cellY >= height_)
        {
            throw std::invalid_argument(
                "M22 scatter instance lies outside the M29 debug page.");
        }

        const auto i =
            static_cast<std::size_t>(instance.cellY) *
                width_ +
            instance.cellX;
        density[i] += 1.0F / cellArea;
    }
}

void TerrainDebugPageData::SetScalar(
    const TerrainDebugField field,
    const std::span<const f32> values)
{
    const auto& descriptor = Descriptor(field);
    if (descriptor.valueClass != TerrainDebugValueClass::Scalar &&
        descriptor.valueClass != TerrainDebugValueClass::SignedScalar)
    {
        throw std::invalid_argument(
            "Terrain debug field does not accept scalar page data.");
    }

    RequireTexelCount(values.size(), "scalar");
    auto& storage = fields_[Index(field)];
    storage.Clear();
    storage.scalar.assign(
        values.begin(),
        values.end());
}

void TerrainDebugPageData::SetVector(
    const TerrainDebugField field,
    const std::span<const TerrainDebugVector2> values)
{
    if (Descriptor(field).valueClass !=
        TerrainDebugValueClass::Vector)
    {
        throw std::invalid_argument(
            "Terrain debug field does not accept vector page data.");
    }

    RequireTexelCount(values.size(), "vector");
    auto& storage = fields_[Index(field)];
    storage.Clear();
    storage.vector.assign(
        values.begin(),
        values.end());
}

void TerrainDebugPageData::SetCategory(
    const TerrainDebugField field,
    const std::span<const u32> values)
{
    if (Descriptor(field).valueClass !=
        TerrainDebugValueClass::Category)
    {
        throw std::invalid_argument(
            "Terrain debug field does not accept category page data.");
    }

    RequireTexelCount(values.size(), "category");
    auto& storage = fields_[Index(field)];
    storage.Clear();
    storage.category.assign(
        values.begin(),
        values.end());
}

void TerrainDebugPageData::CapturePageProvenance()
{
    const auto count =
        static_cast<std::size_t>(
            TexelCount());

    auto& resident =
        fields_[Index(TerrainDebugField::CacheResidency)];
    resident.Clear();
    resident.boolean.assign(
        count,
        stamp_.cacheResident ? 1U : 0U);

    auto& invalidation =
        fields_[Index(TerrainDebugField::CacheInvalidation)];
    invalidation.Clear();
    invalidation.revision.assign(
        count,
        stamp_.invalidationRevision);

    auto& lod =
        fields_[Index(TerrainDebugField::PhysicalLod)];
    lod.Clear();
    lod.lod.assign(
        count,
        stamp_.physicalLod);
}

bool TerrainDebugPageData::Has(
    const TerrainDebugField field) const noexcept
{
    const auto index =
        static_cast<std::size_t>(field);

    if (index >= fields_.size())
    {
        return false;
    }

    return !fields_[index].Empty();
}

TerrainDebugRasterView TerrainDebugPageData::View(
    const TerrainDebugField field,
    const TerrainDebugRasterRange range) const
{
    const auto index = Index(field);
    const auto& storage = fields_[index];

    if (storage.Empty())
    {
        throw std::logic_error(
            "Requested terrain debug field is not bound for this physical page.");
    }

    return {
        .field = field,
        .width = width_,
        .height = height_,
        .range = range,
        .scalar = storage.scalar,
        .vector = storage.vector,
        .category = storage.category,
        .boolean = storage.boolean,
        .revision = storage.revision,
        .lod = storage.lod
    };
}

bool TerrainDebugPageData::FieldStorage::Empty() const noexcept
{
    return
        scalar.empty() &&
        vector.empty() &&
        category.empty() &&
        boolean.empty() &&
        revision.empty() &&
        lod.empty();
}

void TerrainDebugPageData::FieldStorage::Clear()
{
    scalar.clear();
    vector.clear();
    category.clear();
    boolean.clear();
    revision.clear();
    lod.clear();
}

std::size_t TerrainDebugPageData::Index(
    const TerrainDebugField field) const
{
    const auto index =
        static_cast<std::size_t>(field);

    if (index >= fields_.size())
    {
        throw std::out_of_range(
            "Unknown terrain debug field.");
    }

    return index;
}

u64 TerrainDebugPageData::TexelCount() const noexcept
{
    return
        static_cast<u64>(width_) *
        static_cast<u64>(height_);
}

void TerrainDebugPageData::RequireTexelCount(
    const std::size_t size,
    const char* const source) const
{
    if (static_cast<u64>(size) != TexelCount())
    {
        throw std::invalid_argument(
            std::string("Terrain debug ") +
            source +
            " texel count does not match physical page dimensions.");
    }
}
} // namespace orbit::terrain_debug
