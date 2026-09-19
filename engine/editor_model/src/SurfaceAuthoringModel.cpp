#include <orbit/editor_model/SurfaceAuthoringModel.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <variant>

namespace orbit::editor_model
{
namespace
{
template <typename Value>
[[nodiscard]] Value PropertyOr(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    Value fallback)
{
    const auto stored = objects.GetProperty(object, property);
    if (!stored.has_value())
    {
        return fallback;
    }

    const auto* value = std::get_if<Value>(&*stored);
    if (value == nullptr)
    {
        throw std::runtime_error(
            "Surface authoring property has an unexpected persisted type.");
    }

    return *value;
}

[[nodiscard]] bool FiniteNonNegative(const f64 value) noexcept
{
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] bool ValidBand(
    const SurfaceBiomePreferenceBand& band,
    const terrain_biome::BiomeSelectorField field) noexcept
{
    terrain_biome::BiomeAutomaticSelector selector{
        .field = field,
        .minimum = band.minimum,
        .maximum = band.maximum,
        .lowerFalloff = band.lowerFalloff,
        .upperFalloff = band.upperFalloff
    };
    return selector.IsValid();
}

[[nodiscard]] const char* SelectorName(
    const terrain_biome::BiomeSelectorField field) noexcept
{
    switch (field)
    {
    case terrain_biome::BiomeSelectorField::Temperature:
        return "Temperature Preference";
    case terrain_biome::BiomeSelectorField::Moisture:
        return "Moisture Preference";
    case terrain_biome::BiomeSelectorField::Elevation:
        return "Elevation Preference";
    default:
        return "Biome Selector";
    }
}

[[nodiscard]] i64 SelectorSortOrder(
    const terrain_biome::BiomeSelectorField field) noexcept
{
    switch (field)
    {
    case terrain_biome::BiomeSelectorField::Temperature:
        return 10;
    case terrain_biome::BiomeSelectorField::Moisture:
        return 20;
    case terrain_biome::BiomeSelectorField::Elevation:
        return 30;
    default:
        return 100;
    }
}

[[nodiscard]] const char* SurfaceLayerName(
    const terrain_biome::BiomeSurfaceLayerKind kind) noexcept
{
    switch (kind)
    {
    case terrain_biome::BiomeSurfaceLayerKind::Snow:
        return "Snow Surface";
    case terrain_biome::BiomeSurfaceLayerKind::Moss:
        return "Moss Surface";
    case terrain_biome::BiomeSurfaceLayerKind::Litter:
        return "Litter Surface";
    case terrain_biome::BiomeSurfaceLayerKind::Dust:
        return "Dust Surface";
    }
    return "Surface Layer";
}

[[nodiscard]] const char* ScatterRuleName(
    const terrain_biome::BiomeScatterKind kind) noexcept
{
    switch (kind)
    {
    case terrain_biome::BiomeScatterKind::Tree:
        return "Tree Scatter";
    case terrain_biome::BiomeScatterKind::Shrub:
        return "Shrub Scatter";
    case terrain_biome::BiomeScatterKind::Grass:
        return "Grass Scatter";
    case terrain_biome::BiomeScatterKind::Stone:
        return "Stone Scatter";
    case terrain_biome::BiomeScatterKind::Debris:
        return "Debris Scatter";
    case terrain_biome::BiomeScatterKind::GroundClutter:
        return "Ground Clutter";
    }
    return "Scatter Rule";
}

struct ScatterDefaults
{
    f64 density{0.01};
    f64 spacing{1.0};
    bool requiresSoil{false};
    f64 minimumSoil{0.0};
    f64 maximumSlope{90.0};
    f64 minimumScale{1.0};
    f64 maximumScale{1.0};
};

[[nodiscard]] ScatterDefaults DefaultsFor(
    const terrain_biome::BiomeScatterKind kind) noexcept
{
    switch (kind)
    {
    case terrain_biome::BiomeScatterKind::Tree:
        return {0.002, 6.0, true, 0.15, 50.0, 0.8, 1.2};
    case terrain_biome::BiomeScatterKind::Shrub:
        return {0.02, 2.0, true, 0.08, 60.0, 0.7, 1.3};
    case terrain_biome::BiomeScatterKind::Grass:
        return {0.5, 0.35, true, 0.02, 65.0, 0.8, 1.2};
    case terrain_biome::BiomeScatterKind::Stone:
        return {0.015, 2.5, false, 0.0, 85.0, 0.6, 1.5};
    case terrain_biome::BiomeScatterKind::Debris:
        return {0.01, 1.5, false, 0.0, 85.0, 0.7, 1.4};
    case terrain_biome::BiomeScatterKind::GroundClutter:
        return {0.2, 0.5, false, 0.0, 80.0, 0.8, 1.2};
    }
    return {};
}
} // namespace

SurfaceAuthoringModel::SurfaceAuthoringModel(
    scene::ObjectStore& objects,
    commands::CommandService& commands,
    selection::SelectionService& selection)
    : objects_(&objects),
      commands_(&commands),
      selection_(&selection)
{
}

std::optional<scene::ObjectId>
SurfaceAuthoringModel::TerrainAncestor(scene::ObjectId object) const
{
    for (u32 depth = 0; depth < 32U; ++depth)
    {
        const auto record = objects_->Find(object);
        if (!record.has_value())
        {
            return std::nullopt;
        }
        if (record->type == world_model::kTerrainSurfaceType)
        {
            return record->id;
        }
        if (!record->parent.has_value())
        {
            return std::nullopt;
        }
        object = *record->parent;
    }
    return std::nullopt;
}

std::optional<SurfaceAuthoringSelection>
SurfaceAuthoringModel::SelectedRockyBody() const
{
    if (selection_->Ordered().size() != 1U)
    {
        return std::nullopt;
    }

    scene::ObjectId cursor = selection_->Ordered().front();
    std::optional<scene::ObjectRecord> body;

    for (u32 depth = 0; depth < 32U; ++depth)
    {
        const auto record = objects_->Find(cursor);
        if (!record.has_value())
        {
            return std::nullopt;
        }
        if (record->type == world_model::kCelestialBodyType)
        {
            body = record;
            break;
        }
        if (!record->parent.has_value())
        {
            return std::nullopt;
        }
        cursor = *record->parent;
    }

    if (!body.has_value())
    {
        return std::nullopt;
    }

    for (const auto& child : objects_->Children(body->id))
    {
        if (child.type == world_model::kTerrainSurfaceType)
        {
            return SurfaceAuthoringSelection{
                .body = body->id,
                .terrain = child.id,
                .bodyName = body->name
            };
        }
    }

    return std::nullopt;
}

scene::ObjectRecord SurfaceAuthoringModel::RequireTerrain(
    const scene::ObjectId terrain) const
{
    const auto record = objects_->Find(terrain);
    if (!record.has_value() ||
        record->type != world_model::kTerrainSurfaceType)
    {
        throw std::invalid_argument(
            "Surface authoring requires a Terrain Surface semantic object.");
    }
    return *record;
}

scene::ObjectRecord SurfaceAuthoringModel::RequireBiome(
    const scene::ObjectId biome) const
{
    const auto record = objects_->Find(biome);
    if (!record.has_value() ||
        record->type != world_model::kBiomeAssetType ||
        !TerrainAncestor(biome).has_value())
    {
        throw std::invalid_argument(
            "Surface authoring requires a Biome Asset under a Terrain Surface.");
    }
    return *record;
}

SurfaceAuthoringCounts SurfaceAuthoringModel::Counts(
    const scene::ObjectId terrain) const
{
    static_cast<void>(RequireTerrain(terrain));
    SurfaceAuthoringCounts result{.semanticRevision = objects_->Revision()};

    for (const auto& child : objects_->Children(terrain))
    {
        if (child.type == world_model::kGeologyAssetType)
        {
            ++result.geologyAssets;
        }
        else if (child.type == world_model::kTerrainProcessAssetType)
        {
            ++result.processAssets;
        }
        else if (child.type == world_model::kBiomeAssetType)
        {
            ++result.optionalBiomes;
        }
    }
    return result;
}

SurfaceReliefSettings SurfaceAuthoringModel::Relief(
    const scene::ObjectId terrain) const
{
    static_cast<void>(RequireTerrain(terrain));
    return {
        .macroAmplitudeMeters = PropertyOr<f64>(*objects_, terrain, world_model::kTerrainMacroAmplitudeMeters, 1'200.0),
        .macroWavelengthMeters = PropertyOr<f64>(*objects_, terrain, world_model::kTerrainMacroWavelengthMeters, 800'000.0),
        .detailAmplitudeMeters = PropertyOr<f64>(*objects_, terrain, world_model::kTerrainDetailAmplitudeMeters, 320.0),
        .detailWavelengthMeters = PropertyOr<f64>(*objects_, terrain, world_model::kTerrainDetailWavelengthMeters, 40'000.0),
        .detailOctaves = PropertyOr<i64>(*objects_, terrain, world_model::kTerrainDetailOctaves, i64{10}),
        .maximumElevationMeters = PropertyOr<f64>(*objects_, terrain, world_model::kTerrainMaximumElevationMeters, 8'000.0)
    };
}

void SurfaceAuthoringModel::SetRelief(
    const scene::ObjectId terrain,
    const SurfaceReliefSettings& settings)
{
    static_cast<void>(RequireTerrain(terrain));
    if (!FiniteNonNegative(settings.macroAmplitudeMeters) ||
        !std::isfinite(settings.macroWavelengthMeters) || settings.macroWavelengthMeters <= 0.0 ||
        !FiniteNonNegative(settings.detailAmplitudeMeters) ||
        !std::isfinite(settings.detailWavelengthMeters) || settings.detailWavelengthMeters <= 0.0 ||
        settings.detailOctaves < 1 || settings.detailOctaves > 16 ||
        !std::isfinite(settings.maximumElevationMeters) || settings.maximumElevationMeters <= 0.0)
    {
        throw std::invalid_argument("Surface relief settings are outside the physical terrain schema.");
    }

    const bool owns = !commands_->HasActiveTransaction();
    if (owns) commands_->BeginTransaction("Edit Surface Relief");
    try
    {
        commands_->SetProperty(terrain, world_model::kTerrainMacroAmplitudeMeters, settings.macroAmplitudeMeters);
        commands_->SetProperty(terrain, world_model::kTerrainMacroWavelengthMeters, settings.macroWavelengthMeters);
        commands_->SetProperty(terrain, world_model::kTerrainDetailAmplitudeMeters, settings.detailAmplitudeMeters);
        commands_->SetProperty(terrain, world_model::kTerrainDetailWavelengthMeters, settings.detailWavelengthMeters);
        commands_->SetProperty(terrain, world_model::kTerrainDetailOctaves, settings.detailOctaves);
        commands_->SetProperty(terrain, world_model::kTerrainMaximumElevationMeters, settings.maximumElevationMeters);
        if (owns) commands_->CommitTransaction();
    }
    catch (...)
    {
        if (owns && commands_->HasActiveTransaction()) commands_->RollbackTransaction();
        throw;
    }
}

std::vector<SurfaceBiomeSummary> SurfaceAuthoringModel::Biomes(
    const scene::ObjectId terrain) const
{
    static_cast<void>(RequireTerrain(terrain));
    std::vector<SurfaceBiomeSummary> result;

    for (const auto& child : objects_->Children(terrain))
    {
        if (child.type != world_model::kBiomeAssetType)
        {
            continue;
        }

        SurfaceBiomeSummary summary{.id = child.id, .name = child.name};
        for (const auto& rule : objects_->Children(child.id))
        {
            if (rule.type == world_model::kBiomeSelectorType) ++summary.selectors;
            else if (rule.type == world_model::kBiomeAuthoredMaskType) ++summary.authoredMasks;
            else if (rule.type == world_model::kBiomeSurfaceLayerType) ++summary.surfaceLayers;
            else if (rule.type == world_model::kBiomeScatterRuleType) ++summary.scatterRules;
        }
        result.push_back(std::move(summary));
    }
    return result;
}

std::optional<scene::ObjectId> SurfaceAuthoringModel::FindSelector(
    const scene::ObjectId biome,
    const terrain_biome::BiomeSelectorField field) const
{
    static_cast<void>(RequireBiome(biome));
    for (const auto& child : objects_->Children(biome))
    {
        if (child.type != world_model::kBiomeSelectorType) continue;
        const i64 stored = PropertyOr<i64>(*objects_, child.id, world_model::kBiomeSelectorField, i64{0});
        if (stored == static_cast<i64>(field)) return child.id;
    }
    return std::nullopt;
}

scene::ObjectId SurfaceAuthoringModel::EnsureSelector(
    const scene::ObjectId biome,
    const terrain_biome::BiomeSelectorField field)
{
    if (const auto existing = FindSelector(biome, field))
    {
        return *existing;
    }

    const auto created = commands_->CreateObject(
        world_model::kBiomeSelectorType,
        SelectorName(field),
        biome,
        SelectorSortOrder(field));
    commands_->SetProperty(created, world_model::kBiomeSelectorField, static_cast<i64>(field));
    return created;
}

scene::ObjectId SurfaceAuthoringModel::AddBiome(
    const scene::ObjectId terrain,
    std::string name)
{
    static_cast<void>(RequireTerrain(terrain));
    if (name.empty()) throw std::invalid_argument("Biome name must not be empty.");

    for (const auto& biome : Biomes(terrain))
    {
        if (biome.name == name)
        {
            throw std::invalid_argument("A biome with this name already exists on the selected planet.");
        }
    }

    const bool owns = !commands_->HasActiveTransaction();
    if (owns) commands_->BeginTransaction("Add Biome");
    try
    {
        const auto biome = commands_->CreateObject(
            world_model::kBiomeAssetType,
            name,
            terrain,
            100 + static_cast<i64>(Biomes(terrain).size()) * 10);

        commands_->SetProperty(biome, world_model::kBiomePlacementMode, i64{0});
        commands_->SetProperty(biome, world_model::kBiomeMinimumResolvedWeight, 0.05);
        commands_->SetProperty(biome, world_model::kBiomeMaterialInfluence, 1.0);
        commands_->SetProperty(biome, world_model::kBiomeScatterDensityMultiplier, 1.0);

        const SurfaceBiomePreferences defaults{};
        const auto setBand = [&](const terrain_biome::BiomeSelectorField field,
                                 const SurfaceBiomePreferenceBand& band)
        {
            const auto selector = EnsureSelector(biome, field);
            commands_->SetProperty(selector, world_model::kBiomeSelectorMinimum, band.minimum);
            commands_->SetProperty(selector, world_model::kBiomeSelectorMaximum, band.maximum);
            commands_->SetProperty(selector, world_model::kBiomeSelectorLowerFalloff, band.lowerFalloff);
            commands_->SetProperty(selector, world_model::kBiomeSelectorUpperFalloff, band.upperFalloff);
            commands_->SetProperty(selector, world_model::kBiomeSelectorEnabled, true);
        };

        setBand(terrain_biome::BiomeSelectorField::Temperature, defaults.temperature);
        setBand(terrain_biome::BiomeSelectorField::Moisture, defaults.moisture);
        setBand(terrain_biome::BiomeSelectorField::Elevation, defaults.elevation);

        if (owns) commands_->CommitTransaction();
        return biome;
    }
    catch (...)
    {
        if (owns && commands_->HasActiveTransaction()) commands_->RollbackTransaction();
        throw;
    }
}

SurfaceBiomePreferences SurfaceAuthoringModel::CommonPreferences(
    const scene::ObjectId biome) const
{
    static_cast<void>(RequireBiome(biome));
    SurfaceBiomePreferences result{};

    const auto read = [&](const terrain_biome::BiomeSelectorField field,
                          SurfaceBiomePreferenceBand& band)
    {
        const auto selector = FindSelector(biome, field);
        if (!selector.has_value()) return;
        band.minimum = PropertyOr<f64>(*objects_, *selector, world_model::kBiomeSelectorMinimum, band.minimum);
        band.maximum = PropertyOr<f64>(*objects_, *selector, world_model::kBiomeSelectorMaximum, band.maximum);
        band.lowerFalloff = PropertyOr<f64>(*objects_, *selector, world_model::kBiomeSelectorLowerFalloff, band.lowerFalloff);
        band.upperFalloff = PropertyOr<f64>(*objects_, *selector, world_model::kBiomeSelectorUpperFalloff, band.upperFalloff);
    };

    read(terrain_biome::BiomeSelectorField::Temperature, result.temperature);
    read(terrain_biome::BiomeSelectorField::Moisture, result.moisture);
    read(terrain_biome::BiomeSelectorField::Elevation, result.elevation);
    return result;
}

void SurfaceAuthoringModel::SetCommonPreferences(
    const scene::ObjectId biome,
    const SurfaceBiomePreferences& preferences)
{
    static_cast<void>(RequireBiome(biome));
    if (!ValidBand(preferences.temperature, terrain_biome::BiomeSelectorField::Temperature) ||
        !ValidBand(preferences.moisture, terrain_biome::BiomeSelectorField::Moisture) ||
        !ValidBand(preferences.elevation, terrain_biome::BiomeSelectorField::Elevation))
    {
        throw std::invalid_argument("Biome preference band is invalid.");
    }

    const bool owns = !commands_->HasActiveTransaction();
    if (owns) commands_->BeginTransaction("Edit Biome Preferences");
    try
    {
        const auto setBand = [&](const terrain_biome::BiomeSelectorField field,
                                 const SurfaceBiomePreferenceBand& band)
        {
            const auto selector = EnsureSelector(biome, field);
            commands_->SetProperty(selector, world_model::kBiomeSelectorMinimum, band.minimum);
            commands_->SetProperty(selector, world_model::kBiomeSelectorMaximum, band.maximum);
            commands_->SetProperty(selector, world_model::kBiomeSelectorLowerFalloff, band.lowerFalloff);
            commands_->SetProperty(selector, world_model::kBiomeSelectorUpperFalloff, band.upperFalloff);
            commands_->SetProperty(selector, world_model::kBiomeSelectorEnabled, true);
        };

        setBand(terrain_biome::BiomeSelectorField::Temperature, preferences.temperature);
        setBand(terrain_biome::BiomeSelectorField::Moisture, preferences.moisture);
        setBand(terrain_biome::BiomeSelectorField::Elevation, preferences.elevation);

        bool hasMask = false;
        for (const auto& child : objects_->Children(biome))
        {
            if (child.type == world_model::kBiomeAuthoredMaskType)
            {
                hasMask = true;
                break;
            }
        }
        commands_->SetProperty(biome, world_model::kBiomePlacementMode, i64{hasMask ? 2 : 0});
        if (owns) commands_->CommitTransaction();
    }
    catch (...)
    {
        if (owns && commands_->HasActiveTransaction()) commands_->RollbackTransaction();
        throw;
    }
}

SurfaceBiomeSettings SurfaceAuthoringModel::BiomeSettings(
    const scene::ObjectId biome) const
{
    static_cast<void>(RequireBiome(biome));
    return {
        .minimumResolvedWeight = PropertyOr<f64>(*objects_, biome, world_model::kBiomeMinimumResolvedWeight, 0.05),
        .materialInfluence = PropertyOr<f64>(*objects_, biome, world_model::kBiomeMaterialInfluence, 1.0),
        .scatterDensityMultiplier = PropertyOr<f64>(*objects_, biome, world_model::kBiomeScatterDensityMultiplier, 1.0),
        .hydraulicErosion = PropertyOr<f64>(*objects_, biome, world_model::kBiomeHydraulicErosionMultiplier, 1.0),
        .thermalTransport = PropertyOr<f64>(*objects_, biome, world_model::kBiomeThermalTransportMultiplier, 1.0),
        .aeolianTransport = PropertyOr<f64>(*objects_, biome, world_model::kBiomeAeolianTransportMultiplier, 1.0),
        .glacialErosion = PropertyOr<f64>(*objects_, biome, world_model::kBiomeGlacialErosionMultiplier, 1.0),
        .coastalErosion = PropertyOr<f64>(*objects_, biome, world_model::kBiomeCoastalErosionMultiplier, 1.0),
        .chemicalWeathering = PropertyOr<f64>(*objects_, biome, world_model::kBiomeChemicalWeatheringMultiplier, 1.0)
    };
}

void SurfaceAuthoringModel::SetBiomeSettings(
    const scene::ObjectId biome,
    const SurfaceBiomeSettings& settings)
{
    static_cast<void>(RequireBiome(biome));
    const terrain_biome::BiomeProcessModifiers modifiers{
        .hydraulicErosion = static_cast<f32>(settings.hydraulicErosion),
        .thermalTransport = static_cast<f32>(settings.thermalTransport),
        .aeolianTransport = static_cast<f32>(settings.aeolianTransport),
        .glacialErosion = static_cast<f32>(settings.glacialErosion),
        .coastalErosion = static_cast<f32>(settings.coastalErosion),
        .chemicalWeathering = static_cast<f32>(settings.chemicalWeathering)
    };

    if (!std::isfinite(settings.minimumResolvedWeight) ||
        settings.minimumResolvedWeight < 0.0 || settings.minimumResolvedWeight > 1.0 ||
        !FiniteNonNegative(settings.materialInfluence) ||
        !FiniteNonNegative(settings.scatterDensityMultiplier) ||
        !modifiers.IsValid())
    {
        throw std::invalid_argument("Biome material/scatter/process settings are invalid.");
    }

    const bool owns = !commands_->HasActiveTransaction();
    if (owns) commands_->BeginTransaction("Edit Biome Settings");
    try
    {
        commands_->SetProperty(biome, world_model::kBiomeMinimumResolvedWeight, settings.minimumResolvedWeight);
        commands_->SetProperty(biome, world_model::kBiomeMaterialInfluence, settings.materialInfluence);
        commands_->SetProperty(biome, world_model::kBiomeScatterDensityMultiplier, settings.scatterDensityMultiplier);
        commands_->SetProperty(biome, world_model::kBiomeHydraulicErosionMultiplier, settings.hydraulicErosion);
        commands_->SetProperty(biome, world_model::kBiomeThermalTransportMultiplier, settings.thermalTransport);
        commands_->SetProperty(biome, world_model::kBiomeAeolianTransportMultiplier, settings.aeolianTransport);
        commands_->SetProperty(biome, world_model::kBiomeGlacialErosionMultiplier, settings.glacialErosion);
        commands_->SetProperty(biome, world_model::kBiomeCoastalErosionMultiplier, settings.coastalErosion);
        commands_->SetProperty(biome, world_model::kBiomeChemicalWeatheringMultiplier, settings.chemicalWeathering);
        if (owns) commands_->CommitTransaction();
    }
    catch (...)
    {
        if (owns && commands_->HasActiveTransaction()) commands_->RollbackTransaction();
        throw;
    }
}

scene::ObjectId SurfaceAuthoringModel::PaintLocalOverride(
    const scene::ObjectId biome,
    math::Double3 centerUnitDirection,
    const f64 innerRadiusMeters,
    const f64 outerRadiusMeters,
    const f64 weight,
    const f64 opacity)
{
    static_cast<void>(RequireBiome(biome));
    const f64 length = math::Length(centerUnitDirection);

    if (!std::isfinite(length) || length <= 1.0e-12 ||
        !FiniteNonNegative(innerRadiusMeters) ||
        !FiniteNonNegative(outerRadiusMeters) ||
        outerRadiusMeters < innerRadiusMeters || outerRadiusMeters <= 0.0 ||
        !std::isfinite(weight) || weight < 0.0 || weight > 1.0 ||
        !std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0)
    {
        throw std::invalid_argument("Local biome override brush is invalid.");
    }

    centerUnitDirection = math::Normalize(centerUnitDirection);
    const bool owns = !commands_->HasActiveTransaction();
    if (owns) commands_->BeginTransaction("Paint Biome Local Override");

    try
    {
        u32 maskCount = 0;
        for (const auto& child : objects_->Children(biome))
            if (child.type == world_model::kBiomeAuthoredMaskType) ++maskCount;

        const auto mask = commands_->CreateObject(
            world_model::kBiomeAuthoredMaskType,
            "Local Override",
            biome,
            1000 + static_cast<i64>(maskCount) * 10);

        commands_->SetProperty(mask, world_model::kBiomeMaskOperation, i64{2});
        commands_->SetProperty(mask, world_model::kBiomeMaskCenter, centerUnitDirection);
        commands_->SetProperty(mask, world_model::kBiomeMaskInnerRadius, innerRadiusMeters);
        commands_->SetProperty(mask, world_model::kBiomeMaskOuterRadius, outerRadiusMeters);
        commands_->SetProperty(mask, world_model::kBiomeMaskGlobal, false);
        commands_->SetProperty(mask, world_model::kBiomeMaskValue, weight);
        commands_->SetProperty(mask, world_model::kBiomeMaskOpacity, opacity);
        commands_->SetProperty(mask, world_model::kBiomeMaskEnabled, true);
        commands_->SetProperty(biome, world_model::kBiomePlacementMode, i64{2});

        if (owns) commands_->CommitTransaction();
        return mask;
    }
    catch (...)
    {
        if (owns && commands_->HasActiveTransaction()) commands_->RollbackTransaction();
        throw;
    }
}

scene::ObjectId SurfaceAuthoringModel::AddSurfaceLayer(
    const scene::ObjectId biome,
    const terrain_biome::BiomeSurfaceLayerKind kind)
{
    static_cast<void>(RequireBiome(biome));
    const bool owns = !commands_->HasActiveTransaction();
    if (owns) commands_->BeginTransaction("Add Biome Surface Layer");

    try
    {
        u32 count = 0;
        for (const auto& child : objects_->Children(biome))
            if (child.type == world_model::kBiomeSurfaceLayerType) ++count;

        const auto layer = commands_->CreateObject(
            world_model::kBiomeSurfaceLayerType,
            SurfaceLayerName(kind),
            biome,
            2000 + static_cast<i64>(count) * 10);

        commands_->SetProperty(layer, world_model::kBiomeSurfaceLayerKind, static_cast<i64>(kind));
        commands_->SetProperty(layer, world_model::kBiomeSurfaceLayerStrength, 1.0);
        commands_->SetProperty(layer, world_model::kBiomeSurfaceLayerCompatibility, i64{31});
        commands_->SetProperty(layer, world_model::kBiomeSurfaceLayerSlopeMin, 0.0);
        commands_->SetProperty(layer, world_model::kBiomeSurfaceLayerSlopeMax, 90.0);
        commands_->SetProperty(layer, world_model::kBiomeSurfaceLayerCurvatureMin, -1.0);
        commands_->SetProperty(layer, world_model::kBiomeSurfaceLayerCurvatureMax, 1.0);
        commands_->SetProperty(layer, world_model::kBiomeSurfaceLayerMoistureMin,
            kind == terrain_biome::BiomeSurfaceLayerKind::Moss ? 0.35 : 0.0);
        commands_->SetProperty(layer, world_model::kBiomeSurfaceLayerMoistureMax, 1.0);
        commands_->SetProperty(layer, world_model::kBiomeSurfaceLayerEnabled, true);

        if (owns) commands_->CommitTransaction();
        return layer;
    }
    catch (...)
    {
        if (owns && commands_->HasActiveTransaction()) commands_->RollbackTransaction();
        throw;
    }
}

scene::ObjectId SurfaceAuthoringModel::AddScatterRule(
    const scene::ObjectId biome,
    const terrain_biome::BiomeScatterKind kind)
{
    static_cast<void>(RequireBiome(biome));
    const ScatterDefaults defaults = DefaultsFor(kind);
    const bool owns = !commands_->HasActiveTransaction();
    if (owns) commands_->BeginTransaction("Add Biome Scatter Rule");

    try
    {
        u32 count = 0;
        for (const auto& child : objects_->Children(biome))
            if (child.type == world_model::kBiomeScatterRuleType) ++count;

        const auto rule = commands_->CreateObject(
            world_model::kBiomeScatterRuleType,
            ScatterRuleName(kind),
            biome,
            3000 + static_cast<i64>(count) * 10);

        commands_->SetProperty(rule, world_model::kBiomeScatterKind, static_cast<i64>(kind));
        commands_->SetProperty(rule, world_model::kBiomeScatterDensity, defaults.density);
        commands_->SetProperty(rule, world_model::kBiomeScatterSpacing, defaults.spacing);
        commands_->SetProperty(rule, world_model::kBiomeScatterSeedSalt, static_cast<i64>(17U + count * 977U));
        commands_->SetProperty(rule, world_model::kBiomeScatterCompatibility, i64{31});
        commands_->SetProperty(rule, world_model::kBiomeScatterRequiresSoil, defaults.requiresSoil);
        commands_->SetProperty(rule, world_model::kBiomeScatterMinimumSoilDepth, defaults.minimumSoil);
        commands_->SetProperty(rule, world_model::kBiomeScatterSlopeMin, 0.0);
        commands_->SetProperty(rule, world_model::kBiomeScatterSlopeMax, defaults.maximumSlope);
        commands_->SetProperty(rule, world_model::kBiomeScatterMoistureMin, 0.0);
        commands_->SetProperty(rule, world_model::kBiomeScatterMoistureMax, 1.0);
        commands_->SetProperty(rule, world_model::kBiomeScatterScaleMin, defaults.minimumScale);
        commands_->SetProperty(rule, world_model::kBiomeScatterScaleMax, defaults.maximumScale);
        commands_->SetProperty(rule, world_model::kBiomeScatterEnabled, true);

        if (owns) commands_->CommitTransaction();
        return rule;
    }
    catch (...)
    {
        if (owns && commands_->HasActiveTransaction()) commands_->RollbackTransaction();
        throw;
    }
}

std::vector<SurfaceBiomeSelectorDetail> SurfaceAuthoringModel::Selectors(
    const scene::ObjectId biome) const
{
    static_cast<void>(RequireBiome(biome));
    std::vector<SurfaceBiomeSelectorDetail> result;

    for (const auto& child : objects_->Children(biome))
    {
        if (child.type != world_model::kBiomeSelectorType) continue;
        const i64 field = PropertyOr<i64>(*objects_, child.id, world_model::kBiomeSelectorField, i64{0});
        if (field < 0 || field > 16) throw std::runtime_error("Biome selector field is outside the implemented catalog.");

        result.push_back({
            .id = child.id,
            .field = static_cast<terrain_biome::BiomeSelectorField>(field),
            .minimum = PropertyOr<f64>(*objects_, child.id, world_model::kBiomeSelectorMinimum, 0.0),
            .maximum = PropertyOr<f64>(*objects_, child.id, world_model::kBiomeSelectorMaximum, 1.0),
            .lowerFalloff = PropertyOr<f64>(*objects_, child.id, world_model::kBiomeSelectorLowerFalloff, 0.0),
            .upperFalloff = PropertyOr<f64>(*objects_, child.id, world_model::kBiomeSelectorUpperFalloff, 0.0),
            .invert = PropertyOr<bool>(*objects_, child.id, world_model::kBiomeSelectorInvert, false),
            .enabled = PropertyOr<bool>(*objects_, child.id, world_model::kBiomeSelectorEnabled, true)
        });
    }
    return result;
}

std::vector<SurfaceBiomeMaskDetail> SurfaceAuthoringModel::Masks(
    const scene::ObjectId biome) const
{
    static_cast<void>(RequireBiome(biome));
    std::vector<SurfaceBiomeMaskDetail> result;

    for (const auto& child : objects_->Children(biome))
    {
        if (child.type != world_model::kBiomeAuthoredMaskType) continue;
        const i64 operation = PropertyOr<i64>(*objects_, child.id, world_model::kBiomeMaskOperation, i64{0});
        if (operation < 0 || operation > 5) throw std::runtime_error("Biome authored mask operation is outside the implemented catalog.");

        result.push_back({
            .id = child.id,
            .operation = static_cast<terrain_biome::BiomeAuthoredWeightOperation>(operation),
            .centerUnitDirection = PropertyOr<math::Double3>(*objects_, child.id, world_model::kBiomeMaskCenter, {0.0, 1.0, 0.0}),
            .innerRadiusMeters = PropertyOr<f64>(*objects_, child.id, world_model::kBiomeMaskInnerRadius, 0.0),
            .outerRadiusMeters = PropertyOr<f64>(*objects_, child.id, world_model::kBiomeMaskOuterRadius, 1'000.0),
            .value = PropertyOr<f64>(*objects_, child.id, world_model::kBiomeMaskValue, 1.0),
            .opacity = PropertyOr<f64>(*objects_, child.id, world_model::kBiomeMaskOpacity, 1.0),
            .enabled = PropertyOr<bool>(*objects_, child.id, world_model::kBiomeMaskEnabled, true)
        });
    }
    return result;
}

void SurfaceAuthoringModel::SelectObject(const scene::ObjectId object)
{
    if (!objects_->Find(object).has_value())
    {
        throw std::invalid_argument("Cannot select an unknown surface-authoring object.");
    }
    const scene::ObjectId selected[] = {object};
    selection_->Set(selected);
}
} // namespace orbit::editor_model
