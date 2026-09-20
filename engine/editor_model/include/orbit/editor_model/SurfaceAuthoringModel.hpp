#pragma once

#include <orbit/commands/CommandService.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/selection/SelectionService.hpp>
#include <orbit/surface_authoring/TerrainConstraints.hpp>
#include <orbit/surface_model/TerrainBodyServices.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>

#include <optional>
#include <string>
#include <vector>

namespace orbit::editor_model
{
struct SurfaceAuthoringSelection
{
    scene::ObjectId body{};
    scene::ObjectId terrain{};
    std::string bodyName;
};

struct SurfaceReliefSettings
{
    f64 macroAmplitudeMeters{1'200.0};
    f64 macroWavelengthMeters{800'000.0};
    f64 detailAmplitudeMeters{320.0};
    f64 detailWavelengthMeters{40'000.0};
    i64 detailOctaves{10};
    f64 maximumElevationMeters{8'000.0};
};

struct SurfaceBiomePreferenceBand
{
    f64 minimum{0.0};
    f64 maximum{1.0};
    f64 lowerFalloff{0.0};
    f64 upperFalloff{0.0};
};

struct SurfaceBiomePreferences
{
    SurfaceBiomePreferenceBand temperature{-50.0, 60.0, 5.0, 5.0};
    SurfaceBiomePreferenceBand moisture{0.0, 1.0, 0.1, 0.1};
    SurfaceBiomePreferenceBand elevation{-12'000.0, 12'000.0, 500.0, 500.0};
};

struct SurfaceBiomeSettings
{
    f64 minimumResolvedWeight{0.05};
    f64 materialInfluence{1.0};
    f64 scatterDensityMultiplier{1.0};
    f64 hydraulicErosion{1.0};
    f64 thermalTransport{1.0};
    f64 aeolianTransport{1.0};
    f64 glacialErosion{1.0};
    f64 coastalErosion{1.0};
    f64 chemicalWeathering{1.0};
};

struct SurfaceBiomeSummary
{
    scene::ObjectId id{};
    std::string name;
    u32 selectors{0};
    u32 authoredMasks{0};
    u32 surfaceLayers{0};
    u32 scatterRules{0};
};

struct SurfaceBiomeSelectorDetail
{
    scene::ObjectId id{};
    terrain_biome::BiomeSelectorField field{
        terrain_biome::BiomeSelectorField::Temperature};
    f64 minimum{0.0};
    f64 maximum{1.0};
    f64 lowerFalloff{0.0};
    f64 upperFalloff{0.0};
    bool invert{false};
    bool enabled{true};
};

struct SurfaceBiomeMaskDetail
{
    scene::ObjectId id{};
    terrain_biome::BiomeAuthoredWeightOperation operation{
        terrain_biome::BiomeAuthoredWeightOperation::Add};
    math::Double3 centerUnitDirection{0.0, 1.0, 0.0};
    f64 innerRadiusMeters{0.0};
    f64 outerRadiusMeters{1'000.0};
    f64 value{1.0};
    f64 opacity{1.0};
    bool enabled{true};
};

enum class SurfaceTerrainConstraintChannel : u8
{
    Height = 0,
    Protection = 1,
    Drainage = 2,
    Material = 3
};

enum class SurfaceTerrainConstraintShape : u8
{
    Brush = 0,
    Spline = 1
};

struct SurfaceTerrainConstraintDetail
{
    scene::ObjectId id{};
    std::string name;
    SurfaceTerrainConstraintChannel channel{SurfaceTerrainConstraintChannel::Height};
    SurfaceTerrainConstraintShape shape{SurfaceTerrainConstraintShape::Brush};
    surface_authoring::ConstraintCompositionMode mode{surface_authoring::ConstraintCompositionMode::Add};
    math::Double3 centerUnitDirection{0.0,1.0,0.0};
    f64 innerRadiusMeters{0.0};
    f64 outerRadiusMeters{1'000.0};
    std::vector<math::Double3> controlUnitDirections;
    f64 halfWidthMeters{500.0};
    f64 falloffMeters{500.0};
    f64 value{0.0};
    f64 opacity{1.0};
    terrain_geology::RockTypeId material{};
    bool enabled{true};
};

struct SurfaceAuthoringCounts
{
    u32 geologyAssets{0};
    u32 processAssets{0};
    u32 optionalBiomes{0};
    u32 terrainConstraints{0};
    u64 semanticRevision{0};
};

class SurfaceAuthoringModel
{
public:
    SurfaceAuthoringModel(
        scene::ObjectStore& objects,
        commands::CommandService& commands,
        selection::SelectionService& selection);

    [[nodiscard]] std::optional<SurfaceAuthoringSelection>
    SelectedRockyBody() const;

    [[nodiscard]] SurfaceAuthoringCounts Counts(
        scene::ObjectId terrain) const;

    [[nodiscard]] SurfaceReliefSettings Relief(
        scene::ObjectId terrain) const;

    void SetRelief(
        scene::ObjectId terrain,
        const SurfaceReliefSettings& settings);

    [[nodiscard]] std::optional<scene::ObjectId>
    ProcessSettingsObject(
        scene::ObjectId terrain) const;

    [[nodiscard]] scene::ObjectId EnsureProcessSettings(
        scene::ObjectId terrain);

    [[nodiscard]] surface_model::TerrainProcessService
    ProcessSettings(
        scene::ObjectId terrain) const;

    void SetProcessSettings(
        scene::ObjectId terrain,
        const surface_model::TerrainProcessService& settings);

    [[nodiscard]] std::vector<SurfaceTerrainConstraintDetail>
    TerrainConstraints(scene::ObjectId terrain) const;

    [[nodiscard]] scene::ObjectId AddHeightBrush(
        scene::ObjectId terrain,
        math::Double3 centerUnitDirection,
        f64 innerRadiusMeters,
        f64 outerRadiusMeters,
        f64 deltaHeightMeters);

    [[nodiscard]] scene::ObjectId AddProtectionBrush(
        scene::ObjectId terrain,
        math::Double3 centerUnitDirection,
        f64 innerRadiusMeters,
        f64 outerRadiusMeters,
        f64 protection);

    [[nodiscard]] scene::ObjectId AddDrainageBrush(
        scene::ObjectId terrain,
        math::Double3 centerUnitDirection,
        f64 innerRadiusMeters,
        f64 outerRadiusMeters,
        f64 guidance);

    [[nodiscard]] scene::ObjectId AddMaterialBrush(
        scene::ObjectId terrain,
        math::Double3 centerUnitDirection,
        f64 innerRadiusMeters,
        f64 outerRadiusMeters,
        terrain_geology::RockTypeId material,
        f64 weight = 1.0);

    [[nodiscard]] scene::ObjectId AddCanyonSpline(
        scene::ObjectId terrain,
        const std::vector<math::Double3>& controlUnitDirections,
        f64 halfWidthMeters,
        f64 falloffMeters,
        f64 depthMeters);

    [[nodiscard]] scene::ObjectId AddRidgeSpline(
        scene::ObjectId terrain,
        const std::vector<math::Double3>& controlUnitDirections,
        f64 halfWidthMeters,
        f64 falloffMeters,
        f64 heightMeters);

    [[nodiscard]] std::vector<SurfaceBiomeSummary> Biomes(
        scene::ObjectId terrain) const;

    [[nodiscard]] scene::ObjectId AddBiome(
        scene::ObjectId terrain,
        std::string name);

    [[nodiscard]] SurfaceBiomePreferences CommonPreferences(
        scene::ObjectId biome) const;

    void SetCommonPreferences(
        scene::ObjectId biome,
        const SurfaceBiomePreferences& preferences);

    [[nodiscard]] SurfaceBiomeSettings BiomeSettings(
        scene::ObjectId biome) const;

    void SetBiomeSettings(
        scene::ObjectId biome,
        const SurfaceBiomeSettings& settings);

    [[nodiscard]] scene::ObjectId PaintBiomeMask(
        scene::ObjectId biome,
        terrain_biome::BiomeAuthoredWeightOperation operation,
        math::Double3 centerUnitDirection,
        f64 innerRadiusMeters,
        f64 outerRadiusMeters,
        f64 weight,
        f64 opacity = 1.0);

    [[nodiscard]] scene::ObjectId PaintLocalOverride(
        scene::ObjectId biome,
        math::Double3 centerUnitDirection,
        f64 innerRadiusMeters,
        f64 outerRadiusMeters,
        f64 weight,
        f64 opacity = 1.0);

    [[nodiscard]] scene::ObjectId AddSurfaceLayer(
        scene::ObjectId biome,
        terrain_biome::BiomeSurfaceLayerKind kind);

    [[nodiscard]] scene::ObjectId AddScatterRule(
        scene::ObjectId biome,
        terrain_biome::BiomeScatterKind kind);

    [[nodiscard]] std::vector<SurfaceBiomeSelectorDetail>
    Selectors(scene::ObjectId biome) const;

    [[nodiscard]] std::vector<SurfaceBiomeMaskDetail>
    Masks(scene::ObjectId biome) const;

    void SelectObject(scene::ObjectId object);

private:
    [[nodiscard]] scene::ObjectRecord RequireTerrain(
        scene::ObjectId terrain) const;

    [[nodiscard]] scene::ObjectRecord RequireBiome(
        scene::ObjectId biome) const;

    [[nodiscard]] std::optional<scene::ObjectId> TerrainAncestor(
        scene::ObjectId object) const;

    [[nodiscard]] scene::ObjectId EnsureSelector(
        scene::ObjectId biome,
        terrain_biome::BiomeSelectorField field);

    [[nodiscard]] std::optional<scene::ObjectId> FindSelector(
        scene::ObjectId biome,
        terrain_biome::BiomeSelectorField field) const;

    [[nodiscard]] scene::ObjectId AddScalarBrush(
        scene::ObjectId terrain,
        std::string name,
        SurfaceTerrainConstraintChannel channel,
        surface_authoring::ConstraintCompositionMode mode,
        math::Double3 centerUnitDirection,
        f64 innerRadiusMeters,
        f64 outerRadiusMeters,
        f64 value);

    [[nodiscard]] scene::ObjectId AddHeightSpline(
        scene::ObjectId terrain,
        std::string name,
        surface_authoring::ConstraintCompositionMode mode,
        const std::vector<math::Double3>& controlUnitDirections,
        f64 halfWidthMeters,
        f64 falloffMeters,
        f64 value);

    scene::ObjectStore* objects_{nullptr};
    commands::CommandService* commands_{nullptr};
    selection::SelectionService* selection_{nullptr};
};
} // namespace orbit::editor_model
