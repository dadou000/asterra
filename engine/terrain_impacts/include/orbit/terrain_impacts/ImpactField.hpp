#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainPosition.hpp>
#include <orbit/world/Planet.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::terrain_impacts
{
struct ImpactFieldIdTag;
using ImpactFieldId = core::StrongId<ImpactFieldIdTag>;

struct ImpactIdTag;
using ImpactId = core::StrongId<ImpactIdTag>;

enum class CraterProfileKind : u8
{
    Auto,
    Simple,
    Complex
};

struct CraterSizeFrequencyDistribution
{
    u32 count{0};
    f64 minimumRadiusMeters{1'000.0};
    f64 maximumRadiusMeters{100'000.0};

    // Cumulative power-law slope: N(>R) ~ R^-b.
    f64 cumulativeExponent{2.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ImpactRecord
{
    ImpactId id{};
    math::Double3 centerUnitDirection{0.0, 1.0, 0.0};
    f64 radiusMeters{10'000.0};

    CraterProfileKind profile{CraterProfileKind::Auto};

    // Ratios are relative to crater radius.
    f64 simpleDepthRatio{0.18};
    f64 complexDepthRatio{0.075};
    f64 rimHeightRatio{0.035};
    f64 ejectaThicknessRatio{0.012};
    f64 ejectaExtentRadii{3.0};

    // Optional ejecta-ray/debris modulation.
    f64 rayStrength{0.0};
    u32 rayCount{0};

    // 0 = pristine, 1 = fully topographically degraded.
    f64 degradation{0.0};

    // Higher values are younger. This is preserved for later process
    // sequencing even though M07 height deltas compose additively.
    u64 ageOrder{0};

    bool enabled{true};
    bool authored{true};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ImpactFieldDefinition
{
    ImpactFieldId id{};
    world::PlanetId planet{};
    std::string name;

    // Zero derives from PlanetDefinition::generationSeed.
    u64 seed{0};

    CraterSizeFrequencyDistribution procedural{};
    f64 complexTransitionRadiusMeters{18'000.0};

    std::vector<ImpactRecord> authoredImpacts;

    [[nodiscard]] bool IsValid() const noexcept;
};

struct CraterProcessSample
{
    // Signed bedrock/terrain delta: negative excavation plus positive rim/ejecta.
    f64 heightDeltaMeters{0.0};

    // Positive process channels for M08+ material-column coupling.
    f64 excavationDepthMeters{0.0};
    f64 ejectaThicknessMeters{0.0};
    f64 debrisField{0.0};
    f64 rayField{0.0};

    u32 affectingImpacts{0};
};

class ImpactField
{
public:
    ImpactField(
        world::PlanetDefinition planet,
        ImpactFieldDefinition definition);

    [[nodiscard]] CraterProcessSample Sample(
        const terrain::PlanetSurfacePosition& position,
        const terrain::TerrainSampleFootprint& footprint) const;

    [[nodiscard]] const ImpactFieldDefinition&
    Definition() const noexcept;

    // Includes deterministic procedural impacts followed by authored impacts.
    [[nodiscard]] const std::vector<ImpactRecord>&
    ResolvedImpacts() const noexcept;

private:
    world::PlanetDefinition planet_{};
    ImpactFieldDefinition definition_{};
    std::vector<ImpactRecord> resolvedImpacts_;
};

[[nodiscard]] ImpactFieldDefinition
MakeMoonLikeImpactPreset(
    world::PlanetId planet,
    ImpactFieldId fieldId,
    u64 seed = 0);

// Project-authority codec for .orbitimpacts records.
[[nodiscard]] ImpactFieldDefinition ParseImpactFieldToml(
    std::string_view text);
[[nodiscard]] std::string SerializeImpactFieldToml(
    const ImpactFieldDefinition& definition);
[[nodiscard]] ImpactFieldDefinition LoadImpactFieldFile(
    const std::filesystem::path& path);
} // namespace orbit::terrain_impacts
