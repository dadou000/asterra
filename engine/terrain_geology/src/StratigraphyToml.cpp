#include <orbit/terrain_geology/Stratigraphy.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::terrain_geology
{
namespace
{
[[nodiscard]] std::string RequiredString(
    const toml::table& table,
    const std::string_view key)
{
    const auto value =
        table[key].value<std::string>();

    if (!value.has_value() ||
        value->empty())
    {
        throw std::runtime_error(
            "Stratigraphy profile is missing non-empty string field '" +
            std::string(key) +
            "'.");
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

[[nodiscard]] RockTypeId ParseRockType(
    const std::string& text,
    const std::string_view field)
{
    const auto parsed =
        RockTypeId::Parse(text);

    if (!parsed.has_value() ||
        !parsed->IsValid())
    {
        throw std::runtime_error(
            "Stratigraphy field '" +
            std::string(field) +
            "' is not a valid RockTypeId.");
    }

    return *parsed;
}

[[nodiscard]] bool IsStratigraphyPath(
    const std::filesystem::path& path)
{
    std::string extension =
        path.extension().string();

    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const unsigned char value)
        {
            return static_cast<char>(
                std::tolower(value));
        });

    return extension == ".orbitstratigraphy";
}
} // namespace

StratigraphyProfile ParseStratigraphyProfileToml(
    const std::string_view text)
{
    const toml::table document =
        toml::parse(text);

    const toml::table* table =
        document["stratigraphy"].as_table();

    if (table == nullptr)
    {
        throw std::runtime_error(
            "Stratigraphy asset requires a [stratigraphy] table.");
    }

    const std::string idText =
        RequiredString(*table, "id");
    const auto profileId =
        StratigraphyProfileId::Parse(idText);

    if (!profileId.has_value() ||
        !profileId->IsValid())
    {
        throw std::runtime_error(
            "Stratigraphy id is not a valid stable Orbit ID.");
    }

    StratigraphyProfile profile{
        .id = *profileId,
        .name = RequiredString(*table, "name"),
        .layers = {},
        .basementMaterial =
            ParseRockType(
                RequiredString(
                    *table,
                    "basement_material"),
                "basement_material"),
        .transform = {}
    };

    if (const toml::table* transform =
            (*table)["transform"].as_table();
        transform != nullptr)
    {
        profile.transform.anchorUnitDirection = {
            OptionalFloat(*transform, "anchor_x", 0.0),
            OptionalFloat(*transform, "anchor_y", 1.0),
            OptionalFloat(*transform, "anchor_z", 0.0)
        };
        profile.transform.referenceTopRadialOffsetMeters =
            OptionalFloat(
                *transform,
                "reference_top_radial_offset_m",
                0.0);
        profile.transform.tiltEastSlope =
            OptionalFloat(
                *transform,
                "tilt_east_slope",
                0.0);
        profile.transform.tiltNorthSlope =
            OptionalFloat(
                *transform,
                "tilt_north_slope",
                0.0);
        profile.transform.foldAmplitudeMeters =
            OptionalFloat(
                *transform,
                "fold_amplitude_m",
                0.0);
        profile.transform.foldWavelengthMeters =
            OptionalFloat(
                *transform,
                "fold_wavelength_m",
                1'000.0);
        profile.transform.foldAzimuthRadians =
            OptionalFloat(
                *transform,
                "fold_azimuth_rad",
                0.0);
        profile.transform.foldPhaseRadians =
            OptionalFloat(
                *transform,
                "fold_phase_rad",
                0.0);
        profile.transform.warpAmplitudeMeters =
            OptionalFloat(
                *transform,
                "warp_amplitude_m",
                0.0);
        profile.transform.warpWavelengthMeters =
            OptionalFloat(
                *transform,
                "warp_wavelength_m",
                1'000.0);
        profile.transform.warpPhaseRadians =
            OptionalFloat(
                *transform,
                "warp_phase_rad",
                0.0);
    }

    if (const toml::array* layers =
            (*table)["layers"].as_array();
        layers != nullptr)
    {
        profile.layers.reserve(layers->size());

        for (const toml::node& node : *layers)
        {
            const toml::table* layer =
                node.as_table();

            if (layer == nullptr)
            {
                throw std::runtime_error(
                    "Each stratigraphy layer must be a table.");
            }

            const f64 thickness =
                OptionalFloat(
                    *layer,
                    "thickness_m",
                    -1.0);

            profile.layers.push_back({
                .material =
                    ParseRockType(
                        RequiredString(
                            *layer,
                            "material"),
                        "layers.material"),
                .thicknessMeters = thickness,
                .transitionBandMeters =
                    OptionalFloat(
                        *layer,
                        "transition_band_m",
                        0.0)
            });
        }
    }

    if (!profile.IsValid())
    {
        throw std::runtime_error(
            "Stratigraphy profile contains invalid layer or deformation values.");
    }

    return profile;
}

std::string SerializeStratigraphyProfileToml(
    const StratigraphyProfile& profile)
{
    if (!profile.IsValid())
    {
        throw std::invalid_argument(
            "Cannot serialize an invalid stratigraphy profile.");
    }

    toml::table transform;
    transform.insert(
        "anchor_x",
        profile.transform.anchorUnitDirection.x);
    transform.insert(
        "anchor_y",
        profile.transform.anchorUnitDirection.y);
    transform.insert(
        "anchor_z",
        profile.transform.anchorUnitDirection.z);
    transform.insert(
        "reference_top_radial_offset_m",
        profile.transform.referenceTopRadialOffsetMeters);
    transform.insert(
        "tilt_east_slope",
        profile.transform.tiltEastSlope);
    transform.insert(
        "tilt_north_slope",
        profile.transform.tiltNorthSlope);
    transform.insert(
        "fold_amplitude_m",
        profile.transform.foldAmplitudeMeters);
    transform.insert(
        "fold_wavelength_m",
        profile.transform.foldWavelengthMeters);
    transform.insert(
        "fold_azimuth_rad",
        profile.transform.foldAzimuthRadians);
    transform.insert(
        "fold_phase_rad",
        profile.transform.foldPhaseRadians);
    transform.insert(
        "warp_amplitude_m",
        profile.transform.warpAmplitudeMeters);
    transform.insert(
        "warp_wavelength_m",
        profile.transform.warpWavelengthMeters);
    transform.insert(
        "warp_phase_rad",
        profile.transform.warpPhaseRadians);

    toml::array layers;
    for (const StratigraphyLayer& layer : profile.layers)
    {
        toml::table authoredLayer;
        authoredLayer.insert(
            "material",
            layer.material.ToString());
        authoredLayer.insert(
            "thickness_m",
            layer.thicknessMeters);
        authoredLayer.insert(
            "transition_band_m",
            layer.transitionBandMeters);
        layers.push_back(
            std::move(authoredLayer));
    }

    toml::table authored;
    authored.insert("id", profile.id.ToString());
    authored.insert("name", profile.name);
    authored.insert(
        "basement_material",
        profile.basementMaterial.ToString());
    authored.insert(
        "transform",
        std::move(transform));
    authored.insert(
        "layers",
        std::move(layers));

    toml::table document;
    document.insert(
        "stratigraphy",
        std::move(authored));

    std::ostringstream stream;
    stream << document;
    return stream.str();
}

StratigraphyProfile LoadStratigraphyProfileFile(
    const std::filesystem::path& path)
{
    if (!std::filesystem::is_regular_file(path) ||
        !IsStratigraphyPath(path))
    {
        throw std::invalid_argument(
            "Stratigraphy authority path must be a .orbitstratigraphy file: " +
            path.string());
    }

    std::ifstream stream(
        path,
        std::ios::binary);

    if (!stream)
    {
        throw std::runtime_error(
            "Failed to open stratigraphy authority file: " +
            path.string());
    }

    std::ostringstream contents;
    contents << stream.rdbuf();

    try
    {
        return ParseStratigraphyProfileToml(
            contents.str());
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            "Failed to parse stratigraphy authority file '" +
            path.string() +
            "': " +
            exception.what());
    }
}
} // namespace orbit::terrain_geology
