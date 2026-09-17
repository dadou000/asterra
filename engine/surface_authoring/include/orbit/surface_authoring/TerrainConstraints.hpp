#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainPosition.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/world/Planet.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace orbit::surface_authoring
{
struct TerrainConstraintSetIdTag;
using TerrainConstraintSetId =
    core::StrongId<TerrainConstraintSetIdTag>;

struct TerrainConstraintIdTag;
using TerrainConstraintId =
    core::StrongId<TerrainConstraintIdTag>;

enum class ConstraintCompositionMode : u8
{
    Add,
    Subtract,
    Replace,
    Multiply,
    Min,
    Max
};

struct PointConstraintPrimitive
{
    math::Double3 centerUnitDirection{0.0, 1.0, 0.0};
    f64 radiusMeters{1.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct BrushConstraintPrimitive
{
    math::Double3 centerUnitDirection{0.0, 1.0, 0.0};
    f64 innerRadiusMeters{1.0};
    f64 outerRadiusMeters{2.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct SplineConstraintPrimitive
{
    std::vector<math::Double3> controlUnitDirections;
    f64 halfWidthMeters{1.0};
    f64 falloffMeters{0.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct PolygonConstraintPrimitive
{
    std::vector<math::Double3> verticesUnitDirections;
    f64 falloffMeters{0.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

// Canonical tangent-space raster authority. Importers convert external rasters
// into this representation once; runtime evaluation is independent of camera,
// cube face and clipmap phase. Samples are row-major normalized mask values.
struct RasterMaskConstraintPrimitive
{
    math::Double3 anchorUnitDirection{0.0, 1.0, 0.0};
    f64 rotationRadians{0.0};
    u32 width{0};
    u32 height{0};
    f64 cellSizeMeters{1.0};
    std::vector<f32> samples;

    [[nodiscard]] bool IsValid() const noexcept;
};

using TerrainConstraintPrimitive =
    std::variant<
        PointConstraintPrimitive,
        BrushConstraintPrimitive,
        SplineConstraintPrimitive,
        PolygonConstraintPrimitive,
        RasterMaskConstraintPrimitive>;

struct ScalarTerrainConstraint
{
    TerrainConstraintId id{};
    ConstraintCompositionMode mode{
        ConstraintCompositionMode::Add};
    TerrainConstraintPrimitive primitive{};
    f64 value{0.0};
    f64 opacity{1.0};
    bool enabled{true};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct GradientTerrainConstraint
{
    TerrainConstraintId id{};
    ConstraintCompositionMode mode{
        ConstraintCompositionMode::Add};
    TerrainConstraintPrimitive primitive{};
    math::Double2 value{};
    f64 opacity{1.0};
    bool enabled{true};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct MaterialTerrainConstraint
{
    TerrainConstraintId id{};
    ConstraintCompositionMode mode{
        ConstraintCompositionMode::Replace};
    TerrainConstraintPrimitive primitive{};
    terrain_geology::RockTypeId material{};
    f64 weight{1.0};
    f64 opacity{1.0};
    bool enabled{true};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct HeightConstraintField
{
    std::vector<ScalarTerrainConstraint> constraints;
};

struct GradientConstraintField
{
    std::vector<GradientTerrainConstraint> constraints;
};

struct UpliftConstraintField
{
    std::vector<ScalarTerrainConstraint> constraints;
};

struct MaterialConstraintField
{
    std::vector<MaterialTerrainConstraint> constraints;
};

struct ProtectionConstraintField
{
    std::vector<ScalarTerrainConstraint> constraints;
};

struct DrainageConstraintField
{
    std::vector<ScalarTerrainConstraint> constraints;
};

// Project authority. The fields are authored intent; evaluated terrain pages
// are derived and may be regenerated at any time from this set.
struct TerrainConstraintSet
{
    TerrainConstraintSetId id{};
    world::PlanetId planet{};
    std::string name;

    HeightConstraintField height;
    GradientConstraintField gradient;
    UpliftConstraintField uplift;
    MaterialConstraintField material;
    ProtectionConstraintField protection;
    DrainageConstraintField drainage;

    [[nodiscard]] bool IsValid() const noexcept;
};

struct MaterialConstraintSample
{
    static constexpr std::size_t MaxMaterials = 4;

    std::array<terrain_geology::RockTypeId, MaxMaterials> materials{};
    std::array<f32, MaxMaterials> weights{};
    u32 count{0};
};

struct TerrainConstraintBaseline
{
    f64 heightMeters{0.0};
    math::Double2 gradient{};
    f64 upliftMeters{0.0};
    terrain_geology::RockTypeId material{};
    f64 protection{0.0};
    f64 drainage{0.0};
};

struct TerrainConstraintSample
{
    f64 heightMeters{0.0};
    math::Double2 gradient{};
    f64 upliftMeters{0.0};
    MaterialConstraintSample material;
    f64 protection{0.0};
    f64 drainage{0.0};
};

// Returns normalized [0,1] influence for an authored primitive at one M01
// canonical body-space surface position.
[[nodiscard]] f64 EvaluateConstraintInfluence(
    const TerrainConstraintPrimitive& primitive,
    const world::PlanetDefinition& planet,
    const terrain::PlanetSurfacePosition& position) noexcept;

[[nodiscard]] TerrainConstraintSample EvaluateTerrainConstraintSet(
    const TerrainConstraintSet& constraints,
    const world::PlanetDefinition& planet,
    const terrain::PlanetSurfacePosition& position,
    const TerrainConstraintBaseline& baseline = {});

// Protection is a preservation coefficient: 0 means no authored protection,
// 1 means full preservation. Terrain processes multiply their erosion amount
// by this allowance rather than treating authored terrain as an immutable stamp.
[[nodiscard]] f64 ErosionAllowanceFromProtection(
    f64 protection) noexcept;

[[nodiscard]] bool ReferencesKnownMaterials(
    const TerrainConstraintSet& constraints,
    const terrain_geology::GeologicalMaterialLibrary& materials) noexcept;

// Project-authority codec for .orbitterrainconstraints assets.
[[nodiscard]] TerrainConstraintSet ParseTerrainConstraintSetToml(
    std::string_view text);
[[nodiscard]] std::string SerializeTerrainConstraintSetToml(
    const TerrainConstraintSet& constraints);
[[nodiscard]] TerrainConstraintSet LoadTerrainConstraintSetFile(
    const std::filesystem::path& path);
} // namespace orbit::surface_authoring
