#include <orbit/terrain_impacts/ImpactField.hpp>

#include <toml++/toml.hpp>

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace orbit::terrain_impacts
{
namespace
{
template <typename Id>
[[nodiscard]] Id ParseId(
    const std::string& text,
    const std::string_view field)
{
    const auto parsed = Id::Parse(text);
    if (!parsed.has_value() || !parsed->IsValid())
    {
        throw std::runtime_error(
            "Impact field '" + std::string(field) +
            "' is not a valid stable Orbit ID.");
    }
    return *parsed;
}

[[nodiscard]] std::string RequiredString(
    const toml::table& table,
    const std::string_view key)
{
    const auto value = table[key].value<std::string>();
    if (!value.has_value() || value->empty())
    {
        throw std::runtime_error(
            "Impact field is missing non-empty string '" +
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

[[nodiscard]] u32 OptionalU32(
    const toml::table& table,
    const std::string_view key,
    const u32 fallback)
{
    const auto value = table[key].value<i64>();
    if (!value.has_value())
    {
        return fallback;
    }

    if (*value < 0 ||
        static_cast<u64>(*value) >
            std::numeric_limits<u32>::max())
    {
        throw std::runtime_error(
            "Impact field '" + std::string(key) +
            "' is outside the 32-bit unsigned range.");
    }

    return static_cast<u32>(*value);
}

[[nodiscard]] u64 OptionalU64String(
    const toml::table& table,
    const std::string_view key,
    const u64 fallback)
{
    const auto text = table[key].value<std::string>();
    if (!text.has_value())
    {
        const auto integer = table[key].value<i64>();
        if (!integer.has_value())
        {
            return fallback;
        }
        if (*integer < 0)
        {
            throw std::runtime_error(
                "Impact field '" + std::string(key) +
                "' cannot be negative.");
        }
        return static_cast<u64>(*integer);
    }

    try
    {
        std::size_t consumed = 0;
        const u64 value =
            std::stoull(*text, &consumed, 10);
        if (consumed != text->size())
        {
            throw std::runtime_error("trailing");
        }
        return value;
    }
    catch (...)
    {
        throw std::runtime_error(
            "Impact field '" + std::string(key) +
            "' is not a valid unsigned integer.");
    }
}

[[nodiscard]] CraterProfileKind ParseProfile(
    const std::string& value)
{
    if (value == "auto") return CraterProfileKind::Auto;
    if (value == "simple") return CraterProfileKind::Simple;
    if (value == "complex") return CraterProfileKind::Complex;

    throw std::runtime_error(
        "Unknown crater profile kind: " + value);
}

[[nodiscard]] std::string_view ProfileName(
    const CraterProfileKind profile) noexcept
{
    switch (profile)
    {
    case CraterProfileKind::Auto: return "auto";
    case CraterProfileKind::Simple: return "simple";
    case CraterProfileKind::Complex: return "complex";
    }
    return "auto";
}

[[nodiscard]] math::Double3 Direction(
    const toml::table& table)
{
    return {
        OptionalFloat(table, "center_x", 0.0),
        OptionalFloat(table, "center_y", 1.0),
        OptionalFloat(table, "center_z", 0.0)
    };
}
} // namespace

ImpactFieldDefinition ParseImpactFieldToml(
    const std::string_view text)
{
    const toml::table root =
        toml::parse(text);

    ImpactFieldDefinition result{
        .id = ParseId<ImpactFieldId>(
            RequiredString(root, "id"), "id"),
        .planet = ParseId<world::PlanetId>(
            RequiredString(root, "planet"), "planet"),
        .name = RequiredString(root, "name"),
        .seed = OptionalU64String(root, "seed", 0),
        .procedural = {},
        .complexTransitionRadiusMeters =
            OptionalFloat(
                root,
                "complex_transition_radius_m",
                18'000.0)
    };

    if (const toml::table* procedural =
            root["procedural"].as_table())
    {
        result.procedural = {
            .count =
                OptionalU32(
                    *procedural,
                    "count",
                    0),
            .minimumRadiusMeters =
                OptionalFloat(
                    *procedural,
                    "minimum_radius_m",
                    1'000.0),
            .maximumRadiusMeters =
                OptionalFloat(
                    *procedural,
                    "maximum_radius_m",
                    100'000.0),
            .cumulativeExponent =
                OptionalFloat(
                    *procedural,
                    "cumulative_exponent",
                    2.0)
        };
    }

    if (const toml::array* impacts =
            root["impact"].as_array())
    {
        result.authoredImpacts.reserve(
            impacts->size());

        for (const toml::node& node : *impacts)
        {
            const toml::table* table =
                node.as_table();

            if (table == nullptr)
            {
                throw std::runtime_error(
                    "Impact entries must be TOML tables.");
            }

            result.authoredImpacts.push_back({
                .id = ParseId<ImpactId>(
                    RequiredString(*table, "id"),
                    "impact.id"),
                .centerUnitDirection =
                    Direction(*table),
                .radiusMeters =
                    OptionalFloat(
                        *table,
                        "radius_m",
                        -1.0),
                .profile =
                    ParseProfile(
                        table->get_as<std::string>("profile")
                            ? **table->get_as<std::string>("profile")
                            : std::string("auto")),
                .simpleDepthRatio =
                    OptionalFloat(
                        *table,
                        "simple_depth_ratio",
                        0.18),
                .complexDepthRatio =
                    OptionalFloat(
                        *table,
                        "complex_depth_ratio",
                        0.075),
                .rimHeightRatio =
                    OptionalFloat(
                        *table,
                        "rim_height_ratio",
                        0.035),
                .ejectaThicknessRatio =
                    OptionalFloat(
                        *table,
                        "ejecta_thickness_ratio",
                        0.012),
                .ejectaExtentRadii =
                    OptionalFloat(
                        *table,
                        "ejecta_extent_radii",
                        3.0),
                .rayStrength =
                    OptionalFloat(
                        *table,
                        "ray_strength",
                        0.0),
                .rayCount =
                    OptionalU32(
                        *table,
                        "ray_count",
                        0),
                .degradation =
                    OptionalFloat(
                        *table,
                        "degradation",
                        0.0),
                .ageOrder =
                    OptionalU64String(
                        *table,
                        "age_order",
                        0),
                .enabled =
                    OptionalBool(
                        *table,
                        "enabled",
                        true),
                .authored = true
            });
        }
    }

    if (!result.IsValid())
    {
        throw std::runtime_error(
            "Parsed M07 impact field is invalid.");
    }

    return result;
}

std::string SerializeImpactFieldToml(
    const ImpactFieldDefinition& definition)
{
    if (!definition.IsValid())
    {
        throw std::invalid_argument(
            "Cannot serialize invalid M07 impact field.");
    }

    toml::table root;
    root.insert("id", definition.id.ToString());
    root.insert("planet", definition.planet.ToString());
    root.insert("name", definition.name);
    root.insert("seed", std::to_string(definition.seed));
    root.insert(
        "complex_transition_radius_m",
        definition.complexTransitionRadiusMeters);

    toml::table procedural;
    procedural.insert(
        "count",
        static_cast<i64>(
            definition.procedural.count));
    procedural.insert(
        "minimum_radius_m",
        definition.procedural.minimumRadiusMeters);
    procedural.insert(
        "maximum_radius_m",
        definition.procedural.maximumRadiusMeters);
    procedural.insert(
        "cumulative_exponent",
        definition.procedural.cumulativeExponent);
    root.insert(
        "procedural",
        std::move(procedural));

    toml::array impacts;

    for (const ImpactRecord& impact :
         definition.authoredImpacts)
    {
        toml::table table;
        table.insert("id", impact.id.ToString());
        table.insert(
            "center_x",
            impact.centerUnitDirection.x);
        table.insert(
            "center_y",
            impact.centerUnitDirection.y);
        table.insert(
            "center_z",
            impact.centerUnitDirection.z);
        table.insert(
            "radius_m",
            impact.radiusMeters);
        table.insert(
            "profile",
            ProfileName(impact.profile));
        table.insert(
            "simple_depth_ratio",
            impact.simpleDepthRatio);
        table.insert(
            "complex_depth_ratio",
            impact.complexDepthRatio);
        table.insert(
            "rim_height_ratio",
            impact.rimHeightRatio);
        table.insert(
            "ejecta_thickness_ratio",
            impact.ejectaThicknessRatio);
        table.insert(
            "ejecta_extent_radii",
            impact.ejectaExtentRadii);
        table.insert(
            "ray_strength",
            impact.rayStrength);
        table.insert(
            "ray_count",
            static_cast<i64>(
                impact.rayCount));
        table.insert(
            "degradation",
            impact.degradation);
        table.insert(
            "age_order",
            std::to_string(impact.ageOrder));
        table.insert(
            "enabled",
            impact.enabled);

        impacts.push_back(
            std::move(table));
    }

    root.insert(
        "impact",
        std::move(impacts));

    std::ostringstream stream;
    stream << root;
    return stream.str();
}

ImpactFieldDefinition LoadImpactFieldFile(
    const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream)
    {
        throw std::runtime_error(
            "Unable to open M07 impact field: " +
            path.string());
    }

    std::ostringstream text;
    text << stream.rdbuf();

    return ParseImpactFieldToml(
        text.str());
}
} // namespace orbit::terrain_impacts
