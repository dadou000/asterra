#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orbit::terrain_biome
{
struct BiomeIdTag;
using BiomeId = core::StrongId<BiomeIdTag>;

struct BiomeUserFieldIdTag;
using BiomeUserFieldId = core::StrongId<BiomeUserFieldIdTag>;

struct BiomeAuthoredMaskIdTag;
using BiomeAuthoredMaskId = core::StrongId<BiomeAuthoredMaskIdTag>;

enum class BiomePlacementMode : u8
{
    Automatic,
    Authored,
    AutomaticAndAuthored
};

enum class BiomeSelectorField : u8
{
    Temperature,
    Moisture,
    Rainfall,
    Elevation,
    Slope,
    Aspect,
    Latitude,
    Continentality,
    DistanceToCoastWater,
    Drainage,
    SoilDepth,
    SandDepth,
    GeologyMaterial,
    SolarExposure,
    WindExposure,
    SnowPersistence,
    UserField
};

enum class BiomeAuthoredWeightOperation : u8
{
    Add,
    Subtract,
    Replace,
    Multiply,
    Min,
    Max
};

struct BiomeAutomaticSelector
{
    BiomeSelectorField field{
        BiomeSelectorField::Temperature};

    // Smooth plateau: weight is 1 inside [minimum, maximum]. Lower/upper
    // falloff extend the transition outside the plateau.
    f64 minimum{0.0};
    f64 maximum{1.0};
    f64 lowerFalloff{0.0};
    f64 upperFalloff{0.0};

    // Used only by GeologyMaterial.
    terrain_geology::RockTypeId material{};

    // Used only by UserField.
    BiomeUserFieldId userField{};

    bool invert{false};
    bool enabled{true};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct BiomeAuthoredMask
{
    BiomeAuthoredMaskId id{};

    BiomeAuthoredWeightOperation operation{
        BiomeAuthoredWeightOperation::Add};

    // Canonical body-space brush. innerRadius is full influence; outerRadius
    // is zero influence. A radius of zero is permitted for global masks only
    // when global=true.
    math::Double3 centerUnitDirection{0.0, 1.0, 0.0};
    f64 innerRadiusMeters{0.0};
    f64 outerRadiusMeters{0.0};
    bool global{false};

    f64 value{1.0};
    f64 opacity{1.0};
    bool enabled{true};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct BiomeUserFieldValue
{
    BiomeUserFieldId id{};
    f64 value{0.0};
};

struct BiomePlacementContext
{
    // Canonical physical position / planet metric.
    math::Double3 unitDirection{0.0, 1.0, 0.0};
    f64 planetRadiusMeters{1.0};

    f64 temperatureC{15.0};
    f64 moisture{0.5};
    f64 rainfall{0.5};
    f64 elevationMeters{0.0};
    f64 slopeDegrees{0.0};
    f64 aspectRadians{0.0};
    f64 latitudeRadians{0.0};
    f64 continentality{0.5};
    f64 distanceToCoastWaterMeters{0.0};
    f64 drainage{0.0};
    f64 soilDepthMeters{0.0};
    f64 sandDepthMeters{0.0};

    terrain_geology::RockTypeId substrateRock{};

    f64 solarExposure{0.5};
    f64 windExposure{0.5};
    f64 snowPersistence{0.0};

    std::span<const BiomeUserFieldValue> userFields{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct BiomePlacementEvaluation
{
    f64 automaticWeight{0.0};
    f64 authoredWeight{0.0};
    f64 finalWeight{0.0};
};

// Stable arbitrary-user-field identity for authored/plugin-provided scalar
// fields. The name is hashed deterministically; it is project/session
// independent and contains no runtime/cache address.
[[nodiscard]] BiomeUserFieldId BiomeUserFieldIdFromName(
    std::string_view name) noexcept;

struct BiomePlacementRules
{
    // A non-base biome must reach this weight before it participates in the
    // resolved coverage. M20 extends this record with automatic/authored
    // selectors; the threshold remains the final eligibility gate.
    f32 minimumResolvedWeight{0.05F};
    bool enabled{true};

    BiomePlacementMode mode{
        BiomePlacementMode::Automatic};

    std::vector<BiomeAutomaticSelector> selectors;
    std::vector<BiomeAuthoredMask> authoredMasks;

    [[nodiscard]] bool IsValid() const noexcept;
};

enum class BiomeSurfaceLayerKind : u8
{
    Snow,
    Moss,
    Litter,
    Dust
};

enum class BiomeExposedMaterialMask : u32
{
    None = 0U,
    Bedrock = 1U << 0U,
    Regolith = 1U << 1U,
    Soil = 1U << 2U,
    Sand = 1U << 3U,
    Debris = 1U << 4U,
    All = 31U
};

[[nodiscard]] constexpr BiomeExposedMaterialMask operator|(
    const BiomeExposedMaterialMask a,
    const BiomeExposedMaterialMask b) noexcept
{
    return
        static_cast<BiomeExposedMaterialMask>(
            static_cast<u32>(a) |
            static_cast<u32>(b));
}

struct BiomeSurfaceLayerRule
{
    BiomeSurfaceLayerKind kind{
        BiomeSurfaceLayerKind::Dust};

    f32 strength{1.0F};

    BiomeExposedMaterialMask compatibleExposed{
        BiomeExposedMaterialMask::All};

    f32 minimumSlopeDegrees{0.0F};
    f32 maximumSlopeDegrees{90.0F};
    f32 slopeFalloffDegrees{0.0F};

    f32 minimumCurvature{-1.0F};
    f32 maximumCurvature{1.0F};
    f32 curvatureFalloff{0.0F};

    f32 minimumMoisture{0.0F};
    f32 maximumMoisture{1.0F};
    f32 moistureFalloff{0.0F};

    bool enabled{true};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct BiomeSurfaceRules
{
    // Global multiplier for every authored surface layer in this biome.
    f32 materialInfluence{1.0F};

    // Stable semantic child order is the layer stack order. The shared M21
    // resolver evaluates these rules; shaders receive only the resolved blend.
    std::vector<BiomeSurfaceLayerRule> layers;

    [[nodiscard]] bool IsValid() const noexcept;
};

struct BiomeScatterRules
{
    // Multiplicative density control for the M22 scatter system.
    f32 densityMultiplier{1.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct BiomeProcessModifiers
{
    // Dimensionless multipliers applied only when later process milestones
    // explicitly consume biome influence. Intrinsic M02 geology is never
    // rewritten by these values.
    f32 hydraulicErosion{1.0F};
    f32 thermalTransport{1.0F};
    f32 aeolianTransport{1.0F};
    f32 glacialErosion{1.0F};
    f32 coastalErosion{1.0F};
    f32 chemicalWeathering{1.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct BiomeDefinition
{
    BiomeId id{};
    std::string name;

    BiomePlacementRules placement{};
    BiomeSurfaceRules surface{};
    BiomeScatterRules scatter{};
    BiomeProcessModifiers processModifiers{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct BiomeWeightContribution
{
    BiomeId id{};
    f32 weight{0.0F};
};

struct ResolvedBiomeWeight
{
    BiomeId id{};
    f32 weight{0.0F};
    bool base{false};
};

// One service exists for each terrain-bearing (rocky) body. The BaseBiome is
// stored separately from authored optional biomes, so collection mutation
// cannot accidentally remove or duplicate the fallback.
class BiomeService
{
public:
    explicit BiomeService(
        universe::BodyId body);

    [[nodiscard]] universe::BodyId Body() const noexcept;
    [[nodiscard]] const BiomeDefinition& BaseBiome() const noexcept;

    // Replaces the base biome rules/name but never its stable ID.
    void ConfigureBaseBiome(BiomeDefinition definition);

    void UpsertBiome(BiomeDefinition definition);
    [[nodiscard]] bool RemoveBiome(BiomeId id);

    [[nodiscard]] const BiomeDefinition* Find(
        BiomeId id) const noexcept;

    // Base first, followed by optional biomes in stable ID order.
    [[nodiscard]] std::vector<BiomeDefinition> Definitions() const;

    // Resolves optional coverage and always returns exactly one BaseBiome
    // entry. Unknown/stale optional IDs are ignored so deleting an authored
    // biome cannot leave terrain undefined.
    [[nodiscard]] std::vector<ResolvedBiomeWeight> Resolve(
        std::span<const BiomeWeightContribution> contributions) const;

    [[nodiscard]] BiomePlacementEvaluation EvaluatePlacement(
        const BiomeDefinition& biome,
        const BiomePlacementContext& context) const;

    [[nodiscard]] std::vector<ResolvedBiomeWeight> ResolvePlacement(
        const BiomePlacementContext& context) const;

    [[nodiscard]] u64 Revision() const noexcept;

    [[nodiscard]] static BiomeId BaseBiomeId(
        universe::BodyId body) noexcept;

private:
    universe::BodyId body_{};
    BiomeDefinition baseBiome_{};
    std::unordered_map<BiomeId, BiomeDefinition> biomes_;
    u64 revision_{0};
};
} // namespace orbit::terrain_biome
