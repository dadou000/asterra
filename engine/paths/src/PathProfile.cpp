#include <orbit/paths/PathProfile.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace orbit::paths
{
namespace
{
[[nodiscard]] std::string Lower(
    std::string value)
{
    std::ranges::transform(
        value,
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(
                std::tolower(character));
        });
    return value;
}

[[nodiscard]] PathProfileKind ParseKind(
    std::string value)
{
    value = Lower(std::move(value));

    if (value == "road") return PathProfileKind::Road;
    if (value == "rail") return PathProfileKind::Rail;
    if (value == "waterway") return PathProfileKind::Waterway;
    if (value == "reference") return PathProfileKind::Reference;

    throw std::invalid_argument(
        "Unknown path profile kind: " + value);
}

void Validate(const PathProfile& profile)
{
    if (profile.name.empty())
    {
        throw std::invalid_argument(
            "Path profile name must not be empty.");
    }

    if (profile.widthMeters <= 0.0)
    {
        throw std::invalid_argument(
            "Path profile width must be positive.");
    }

    if (profile.minimumRadiusMeters < 0.0)
    {
        throw std::invalid_argument(
            "Path profile minimum radius must not be negative.");
    }

    if (profile.maximumGrade < 0.0 ||
        profile.maximumGrade > 1.0)
    {
        throw std::invalid_argument(
            "Path profile maximum grade must be in [0, 1].");
    }

    if (profile.terrainCutCost < 0.0 ||
        profile.terrainFillCost < 0.0 ||
        profile.waterCrossingCost < 0.0)
    {
        throw std::invalid_argument(
            "Path routing costs must not be negative.");
    }
}
} // namespace

PathProfile LoadPathProfile(
    const std::filesystem::path& source)
{
    if (source.extension() !=
        ".orbitpathprofile")
    {
        throw std::invalid_argument(
            "Path profile assets require .orbitpathprofile extension.");
    }

    const toml::table document =
        toml::parse_file(source.string());
    const toml::table* table =
        document["path_profile"].as_table();

    if (table == nullptr)
    {
        throw std::invalid_argument(
            "Path profile is missing [path_profile].");
    }

    const auto name =
        (*table)["name"].value<std::string>();
    const auto kind =
        (*table)["kind"].value<std::string>();

    if (!name.has_value() ||
        !kind.has_value())
    {
        throw std::invalid_argument(
            "Path profile requires name and kind.");
    }

    PathProfile profile{
        .name = *name,
        .kind = ParseKind(*kind),
        .widthMeters =
            (*table)["width_meters"].value_or(6.0),
        .lanes =
            static_cast<u32>(
                (*table)["lanes"].value_or<i64>(2)),
        .minimumRadiusMeters =
            (*table)["minimum_radius_meters"].value_or(0.0),
        .maximumGrade =
            (*table)["maximum_grade"].value_or(1.0),
        .allowBridge =
            (*table)["allow_bridge"].value_or(true),
        .allowTunnel =
            (*table)["allow_tunnel"].value_or(true),
        .terrainCutCost =
            (*table)["terrain_cut_cost"].value_or(1.0),
        .terrainFillCost =
            (*table)["terrain_fill_cost"].value_or(1.0),
        .waterCrossingCost =
            (*table)["water_crossing_cost"].value_or(1.0)
    };

    if (const toml::array* fields =
            (*table)["preferred_cost_fields"].as_array();
        fields != nullptr)
    {
        for (const auto& node : *fields)
        {
            if (const auto field =
                    node.value<std::string>();
                field.has_value())
            {
                profile.preferredCostFields.push_back(*field);
            }
        }
    }

    Validate(profile);
    return profile;
}

void SavePathProfile(
    const std::filesystem::path& destination,
    const PathProfile& profile)
{
    if (destination.extension() !=
        ".orbitpathprofile")
    {
        throw std::invalid_argument(
            "Path profile assets require .orbitpathprofile extension.");
    }

    Validate(profile);

    toml::table table;
    table.insert("name", profile.name);
    table.insert(
        "kind",
        std::string(PathProfileKindName(profile.kind)));
    table.insert("width_meters", profile.widthMeters);
    table.insert("lanes", static_cast<i64>(profile.lanes));
    table.insert(
        "minimum_radius_meters",
        profile.minimumRadiusMeters);
    table.insert("maximum_grade", profile.maximumGrade);
    table.insert("allow_bridge", profile.allowBridge);
    table.insert("allow_tunnel", profile.allowTunnel);
    table.insert("terrain_cut_cost", profile.terrainCutCost);
    table.insert("terrain_fill_cost", profile.terrainFillCost);
    table.insert("water_crossing_cost", profile.waterCrossingCost);

    toml::array fields;
    for (const auto& field :
         profile.preferredCostFields)
    {
        fields.push_back(field);
    }
    table.insert(
        "preferred_cost_fields",
        std::move(fields));

    toml::table document;
    document.insert(
        "path_profile",
        std::move(table));

    if (const auto parent =
            destination.parent_path();
        !parent.empty())
    {
        std::filesystem::create_directories(parent);
    }

    std::ofstream output(
        destination,
        std::ios::binary |
        std::ios::trunc);

    if (!output)
    {
        throw std::runtime_error(
            "Unable to create path profile asset.");
    }

    output << document;
    output.flush();

    if (!output)
    {
        throw std::runtime_error(
            "Unable to write path profile asset.");
    }
}

std::string_view PathProfileKindName(
    const PathProfileKind kind) noexcept
{
    switch (kind)
    {
    case PathProfileKind::Road: return "road";
    case PathProfileKind::Rail: return "rail";
    case PathProfileKind::Waterway: return "waterway";
    case PathProfileKind::Reference: return "reference";
    }

    return "reference";
}
} // namespace orbit::paths
