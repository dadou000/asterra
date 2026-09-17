#include <orbit/surface_authoring/TerrainConstraints.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::surface_authoring
{
namespace
{
[[nodiscard]] std::string RequiredString(
    const toml::table& table,
    const std::string_view key)
{
    const auto value = table[key].value<std::string>();
    if (!value.has_value() || value->empty())
    {
        throw std::runtime_error(
            "Terrain constraints are missing non-empty string field '" +
            std::string(key) + "'.");
    }
    return *value;
}

[[nodiscard]] f64 OptionalFloat(
    const toml::table& table,
    const std::string_view key,
    const f64 fallback)
{
    return table[key].value<f64>().value_or(fallback);
}

[[nodiscard]] bool OptionalBool(
    const toml::table& table,
    const std::string_view key,
    const bool fallback)
{
    return table[key].value<bool>().value_or(fallback);
}

[[nodiscard]] u32 RequiredU32(
    const toml::table& table,
    const std::string_view key)
{
    const auto value = table[key].value<i64>();
    if (!value.has_value() ||
        *value <= 0 ||
        static_cast<u64>(*value) >
            std::numeric_limits<u32>::max())
    {
        throw std::runtime_error(
            "Terrain raster field '" +
            std::string(key) +
            "' must be a positive 32-bit integer.");
    }
    return static_cast<u32>(*value);
}

template <typename Id>
[[nodiscard]] Id ParseId(
    const std::string& text,
    const std::string_view field)
{
    const auto parsed = Id::Parse(text);
    if (!parsed.has_value() || !parsed->IsValid())
    {
        throw std::runtime_error(
            "Terrain constraint field '" +
            std::string(field) +
            "' is not a valid stable Orbit ID.");
    }
    return *parsed;
}

[[nodiscard]] ConstraintCompositionMode ParseMode(
    const std::string& text)
{
    if (text == "add") return ConstraintCompositionMode::Add;
    if (text == "subtract") return ConstraintCompositionMode::Subtract;
    if (text == "replace") return ConstraintCompositionMode::Replace;
    if (text == "multiply") return ConstraintCompositionMode::Multiply;
    if (text == "min") return ConstraintCompositionMode::Min;
    if (text == "max") return ConstraintCompositionMode::Max;

    throw std::runtime_error(
        "Unknown terrain constraint composition mode: " + text);
}

[[nodiscard]] std::string_view ModeName(
    const ConstraintCompositionMode mode) noexcept
{
    switch (mode)
    {
    case ConstraintCompositionMode::Add: return "add";
    case ConstraintCompositionMode::Subtract: return "subtract";
    case ConstraintCompositionMode::Replace: return "replace";
    case ConstraintCompositionMode::Multiply: return "multiply";
    case ConstraintCompositionMode::Min: return "min";
    case ConstraintCompositionMode::Max: return "max";
    }
    return "add";
}

[[nodiscard]] math::Double3 Direction(
    const toml::table& table,
    const std::string_view prefix,
    const math::Double3 fallback = {0.0, 1.0, 0.0})
{
    const std::string base(prefix);
    return {
        OptionalFloat(table, base + "_x", fallback.x),
        OptionalFloat(table, base + "_y", fallback.y),
        OptionalFloat(table, base + "_z", fallback.z)
    };
}

[[nodiscard]] std::vector<math::Double3> ParseDirections(
    const toml::table& table,
    const std::string_view key)
{
    const toml::array* array = table[key].as_array();
    if (array == nullptr)
    {
        throw std::runtime_error(
            "Terrain constraint primitive is missing direction array '" +
            std::string(key) + "'.");
    }

    std::vector<math::Double3> result;
    result.reserve(array->size());

    for (const toml::node& node : *array)
    {
        const toml::array* point = node.as_array();
        if (point == nullptr || point->size() != 3)
        {
            throw std::runtime_error(
                "Terrain constraint direction entries must be [x, y, z].");
        }

        const auto x = (*point)[0].value<f64>();
        const auto y = (*point)[1].value<f64>();
        const auto z = (*point)[2].value<f64>();
        if (!x.has_value() || !y.has_value() || !z.has_value())
        {
            throw std::runtime_error(
                "Terrain constraint direction entries must be numeric.");
        }
        result.push_back({*x, *y, *z});
    }

    return result;
}

[[nodiscard]] TerrainConstraintPrimitive ParsePrimitive(
    const toml::table& table)
{
    const std::string kind =
        RequiredString(table, "primitive");

    if (kind == "point")
    {
        return PointConstraintPrimitive{
            .centerUnitDirection = Direction(table, "center"),
            .radiusMeters = OptionalFloat(table, "radius_m", -1.0)
        };
    }

    if (kind == "brush")
    {
        return BrushConstraintPrimitive{
            .centerUnitDirection = Direction(table, "center"),
            .innerRadiusMeters = OptionalFloat(table, "inner_radius_m", -1.0),
            .outerRadiusMeters = OptionalFloat(table, "outer_radius_m", -1.0)
        };
    }

    if (kind == "spline")
    {
        return SplineConstraintPrimitive{
            .controlUnitDirections = ParseDirections(table, "points"),
            .halfWidthMeters = OptionalFloat(table, "half_width_m", -1.0),
            .falloffMeters = OptionalFloat(table, "falloff_m", 0.0)
        };
    }

    if (kind == "polygon")
    {
        return PolygonConstraintPrimitive{
            .verticesUnitDirections = ParseDirections(table, "points"),
            .falloffMeters = OptionalFloat(table, "falloff_m", 0.0)
        };
    }

    if (kind == "raster")
    {
        const toml::array* samples = table["samples"].as_array();
        if (samples == nullptr)
        {
            throw std::runtime_error(
                "Raster constraint requires a samples array.");
        }

        std::vector<f32> values;
        values.reserve(samples->size());
        for (const toml::node& node : *samples)
        {
            const auto value = node.value<f64>();
            if (!value.has_value())
            {
                throw std::runtime_error(
                    "Raster constraint samples must be numeric.");
            }
            values.push_back(static_cast<f32>(*value));
        }

        return RasterMaskConstraintPrimitive{
            .anchorUnitDirection = Direction(table, "anchor"),
            .rotationRadians = OptionalFloat(table, "rotation_rad", 0.0),
            .width = RequiredU32(table, "width"),
            .height = RequiredU32(table, "height"),
            .cellSizeMeters = OptionalFloat(table, "cell_size_m", -1.0),
            .samples = std::move(values)
        };
    }

    throw std::runtime_error(
        "Unknown terrain constraint primitive: " + kind);
}

void InsertDirection(
    toml::table& table,
    const std::string_view prefix,
    const math::Double3& direction)
{
    const std::string base(prefix);
    table.insert(base + "_x", direction.x);
    table.insert(base + "_y", direction.y);
    table.insert(base + "_z", direction.z);
}

void InsertDirections(
    toml::table& table,
    const std::vector<math::Double3>& directions)
{
    toml::array points;
    for (const auto& direction : directions)
    {
        toml::array point;
        point.push_back(direction.x);
        point.push_back(direction.y);
        point.push_back(direction.z);
        points.push_back(std::move(point));
    }
    table.insert("points", std::move(points));
}

void SerializePrimitive(
    toml::table& table,
    const TerrainConstraintPrimitive& primitive)
{
    std::visit(
        [&](const auto& value)
        {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, PointConstraintPrimitive>)
            {
                table.insert("primitive", "point");
                InsertDirection(table, "center", value.centerUnitDirection);
                table.insert("radius_m", value.radiusMeters);
            }
            else if constexpr (std::is_same_v<T, BrushConstraintPrimitive>)
            {
                table.insert("primitive", "brush");
                InsertDirection(table, "center", value.centerUnitDirection);
                table.insert("inner_radius_m", value.innerRadiusMeters);
                table.insert("outer_radius_m", value.outerRadiusMeters);
            }
            else if constexpr (std::is_same_v<T, SplineConstraintPrimitive>)
            {
                table.insert("primitive", "spline");
                table.insert("half_width_m", value.halfWidthMeters);
                table.insert("falloff_m", value.falloffMeters);
                InsertDirections(table, value.controlUnitDirections);
            }
            else if constexpr (std::is_same_v<T, PolygonConstraintPrimitive>)
            {
                table.insert("primitive", "polygon");
                table.insert("falloff_m", value.falloffMeters);
                InsertDirections(table, value.verticesUnitDirections);
            }
            else
            {
                table.insert("primitive", "raster");
                InsertDirection(table, "anchor", value.anchorUnitDirection);
                table.insert("rotation_rad", value.rotationRadians);
                table.insert("width", static_cast<i64>(value.width));
                table.insert("height", static_cast<i64>(value.height));
                table.insert("cell_size_m", value.cellSizeMeters);
                toml::array samples;
                for (const f32 sample : value.samples)
                {
                    samples.push_back(static_cast<f64>(sample));
                }
                table.insert("samples", std::move(samples));
            }
        },
        primitive);
}

template <typename Constraint>
void InsertCommon(
    toml::table& table,
    const Constraint& constraint)
{
    table.insert("id", constraint.id.ToString());
    table.insert("mode", ModeName(constraint.mode));
    table.insert("opacity", constraint.opacity);
    table.insert("enabled", constraint.enabled);
    SerializePrimitive(table, constraint.primitive);
}

[[nodiscard]] ScalarTerrainConstraint ParseScalar(
    const toml::table& table)
{
    return {
        .id = ParseId<TerrainConstraintId>(RequiredString(table, "id"), "id"),
        .mode = ParseMode(RequiredString(table, "mode")),
        .primitive = ParsePrimitive(table),
        .value = OptionalFloat(table, "value", 0.0),
        .opacity = OptionalFloat(table, "opacity", 1.0),
        .enabled = OptionalBool(table, "enabled", true)
    };
}

[[nodiscard]] GradientTerrainConstraint ParseGradient(
    const toml::table& table)
{
    return {
        .id = ParseId<TerrainConstraintId>(RequiredString(table, "id"), "id"),
        .mode = ParseMode(RequiredString(table, "mode")),
        .primitive = ParsePrimitive(table),
        .value = {
            OptionalFloat(table, "value_east", 0.0),
            OptionalFloat(table, "value_north", 0.0)
        },
        .opacity = OptionalFloat(table, "opacity", 1.0),
        .enabled = OptionalBool(table, "enabled", true)
    };
}

[[nodiscard]] MaterialTerrainConstraint ParseMaterial(
    const toml::table& table)
{
    return {
        .id = ParseId<TerrainConstraintId>(RequiredString(table, "id"), "id"),
        .mode = ParseMode(RequiredString(table, "mode")),
        .primitive = ParsePrimitive(table),
        .material = ParseId<terrain_geology::RockTypeId>(
            RequiredString(table, "material"),
            "material"),
        .weight = OptionalFloat(table, "weight", 1.0),
        .opacity = OptionalFloat(table, "opacity", 1.0),
        .enabled = OptionalBool(table, "enabled", true)
    };
}

template <typename Constraint, typename ParseFunction>
void ParseArray(
    const toml::table& root,
    const std::string_view key,
    std::vector<Constraint>& output,
    ParseFunction parse)
{
    const toml::array* array = root[key].as_array();
    if (array == nullptr)
    {
        return;
    }

    output.reserve(array->size());
    for (const toml::node& node : *array)
    {
        const toml::table* table = node.as_table();
        if (table == nullptr)
        {
            throw std::runtime_error(
                "Terrain constraint array entries must be tables.");
        }
        output.push_back(parse(*table));
    }
}

template <typename Constraint, typename FillFunction>
[[nodiscard]] toml::array SerializeArray(
    const std::vector<Constraint>& constraints,
    FillFunction fill)
{
    toml::array array;
    for (const auto& constraint : constraints)
    {
        toml::table table;
        InsertCommon(table, constraint);
        fill(table, constraint);
        array.push_back(std::move(table));
    }
    return array;
}

[[nodiscard]] bool IsTerrainConstraintPath(
    const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](const unsigned char value)
        {
            return static_cast<char>(std::tolower(value));
        });
    return extension == ".orbitterrainconstraints";
}
} // namespace

TerrainConstraintSet ParseTerrainConstraintSetToml(
    const std::string_view text)
{
    const toml::table document = toml::parse(text);
    const toml::table* root =
        document["terrain_constraints"].as_table();
    if (root == nullptr)
    {
        throw std::runtime_error(
            "Terrain constraint asset requires a [terrain_constraints] table.");
    }

    TerrainConstraintSet result{
        .id = ParseId<TerrainConstraintSetId>(
            RequiredString(*root, "id"), "id"),
        .planet = ParseId<world::PlanetId>(
            RequiredString(*root, "planet"), "planet"),
        .name = RequiredString(*root, "name")
    };

    ParseArray(*root, "height", result.height.constraints, ParseScalar);
    ParseArray(*root, "gradient", result.gradient.constraints, ParseGradient);
    ParseArray(*root, "uplift", result.uplift.constraints, ParseScalar);
    ParseArray(*root, "material", result.material.constraints, ParseMaterial);
    ParseArray(*root, "protection", result.protection.constraints, ParseScalar);
    ParseArray(*root, "drainage", result.drainage.constraints, ParseScalar);

    if (!result.IsValid())
    {
        throw std::runtime_error(
            "Terrain constraint set contains invalid or duplicate authored constraints.");
    }

    return result;
}

std::string SerializeTerrainConstraintSetToml(
    const TerrainConstraintSet& constraints)
{
    if (!constraints.IsValid())
    {
        throw std::invalid_argument(
            "Cannot serialize an invalid terrain constraint set.");
    }

    auto scalarFill =
        [](toml::table& table,
           const ScalarTerrainConstraint& constraint)
        {
            table.insert("value", constraint.value);
        };
    auto gradientFill =
        [](toml::table& table,
           const GradientTerrainConstraint& constraint)
        {
            table.insert("value_east", constraint.value.x);
            table.insert("value_north", constraint.value.y);
        };
    auto materialFill =
        [](toml::table& table,
           const MaterialTerrainConstraint& constraint)
        {
            table.insert("material", constraint.material.ToString());
            table.insert("weight", constraint.weight);
        };

    toml::table root;
    root.insert("id", constraints.id.ToString());
    root.insert("planet", constraints.planet.ToString());
    root.insert("name", constraints.name);
    root.insert("height", SerializeArray(constraints.height.constraints, scalarFill));
    root.insert("gradient", SerializeArray(constraints.gradient.constraints, gradientFill));
    root.insert("uplift", SerializeArray(constraints.uplift.constraints, scalarFill));
    root.insert("material", SerializeArray(constraints.material.constraints, materialFill));
    root.insert("protection", SerializeArray(constraints.protection.constraints, scalarFill));
    root.insert("drainage", SerializeArray(constraints.drainage.constraints, scalarFill));

    toml::table document;
    document.insert("terrain_constraints", std::move(root));
    std::ostringstream stream;
    stream << document;
    return stream.str();
}

TerrainConstraintSet LoadTerrainConstraintSetFile(
    const std::filesystem::path& path)
{
    if (!std::filesystem::is_regular_file(path) ||
        !IsTerrainConstraintPath(path))
    {
        throw std::invalid_argument(
            "Terrain constraint authority path must be a .orbitterrainconstraints file: " +
            path.string());
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        throw std::runtime_error(
            "Failed to open terrain constraint authority file: " +
            path.string());
    }

    std::ostringstream contents;
    contents << stream.rdbuf();
    try
    {
        return ParseTerrainConstraintSetToml(contents.str());
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to parse terrain constraint authority file '" +
            path.string() + "': " + exception.what());
    }
}
} // namespace orbit::surface_authoring
