#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace orbit::terrain_biome
{
struct BiomeIdTag;
using BiomeId = core::StrongId<BiomeIdTag>;

struct BiomePlacementRules
{
    // A non-base biome must reach this weight before it participates in the
    // resolved coverage. M20 extends this record with automatic/authored
    // selectors; the threshold remains the final eligibility gate.
    f32 minimumResolvedWeight{0.05F};
    bool enabled{true};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct BiomeSurfaceRules
{
    // Generic multiplier carried into M21's physical-surface × biome material
    // resolver. Zero means the biome contributes no visual surface layer.
    f32 materialInfluence{1.0F};

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
