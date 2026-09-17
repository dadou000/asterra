#include <orbit/terrain_geology/GeologicalMaterial.hpp>

#include <toml++/toml.hpp>

#include <sstream>
#include <stdexcept>
#include <string>

namespace orbit::terrain_geology
{
namespace
{
[[nodiscard]] f32 RequiredFloat(
    const toml::table& table,
    const std::string_view key)
{
    const auto value =
        table[key].value<f64>();

    if (!value.has_value())
    {
        throw std::runtime_error(
            "Geological material is missing numeric field '" +
            std::string(key) +
            "'.");
    }

    return static_cast<f32>(*value);
}
} // namespace

GeologicalMaterial ParseGeologicalMaterialToml(
    const std::string_view text)
{
    const toml::table document =
        toml::parse(text);

    const toml::table* table =
        document["geological_material"].
            as_table();

    if (table == nullptr)
    {
        throw std::runtime_error(
            "Geological material asset requires a [geological_material] table.");
    }

    const auto idText =
        (*table)["id"].value<std::string>();
    const auto name =
        (*table)["name"].value<std::string>();

    if (!idText.has_value() ||
        !name.has_value() ||
        name->empty())
    {
        throw std::runtime_error(
            "Geological material asset requires non-empty id and name fields.");
    }

    const auto id =
        RockTypeId::Parse(*idText);

    if (!id.has_value() ||
        !id->IsValid())
    {
        throw std::runtime_error(
            "Geological material id is not a valid stable Orbit ID.");
    }

    GeologicalMaterial material{
        .id = *id,
        .name = *name,
        .hardness =
            RequiredFloat(
                *table,
                "hardness"),
        .cohesion =
            RequiredFloat(
                *table,
                "cohesion"),
        .hydraulicErodibility =
            RequiredFloat(
                *table,
                "hydraulic_erodibility"),
        .aeolianErodibility =
            RequiredFloat(
                *table,
                "aeolian_erodibility"),
        .permeability =
            RequiredFloat(
                *table,
                "permeability"),
        .chemicalWeatherability =
            RequiredFloat(
                *table,
                "chemical_weatherability"),
        .fractureTendency =
            RequiredFloat(
                *table,
                "fracture_tendency"),
        .density =
            RequiredFloat(
                *table,
                "density")
    };

    if (!material.IsValid())
    {
        throw std::runtime_error(
            "Geological material contains out-of-range physical coefficients.");
    }

    return material;
}

std::string SerializeGeologicalMaterialToml(
    const GeologicalMaterial& material)
{
    if (!material.IsValid())
    {
        throw std::invalid_argument(
            "Cannot serialize an invalid geological material.");
    }

    toml::table authored;
    authored.insert(
        "id",
        material.id.ToString());
    authored.insert(
        "name",
        material.name);
    authored.insert(
        "hardness",
        static_cast<f64>(material.hardness));
    authored.insert(
        "cohesion",
        static_cast<f64>(material.cohesion));
    authored.insert(
        "hydraulic_erodibility",
        static_cast<f64>(
            material.hydraulicErodibility));
    authored.insert(
        "aeolian_erodibility",
        static_cast<f64>(
            material.aeolianErodibility));
    authored.insert(
        "permeability",
        static_cast<f64>(material.permeability));
    authored.insert(
        "chemical_weatherability",
        static_cast<f64>(
            material.chemicalWeatherability));
    authored.insert(
        "fracture_tendency",
        static_cast<f64>(
            material.fractureTendency));
    authored.insert(
        "density",
        static_cast<f64>(material.density));

    toml::table document;
    document.insert(
        "geological_material",
        std::move(authored));

    std::ostringstream stream;
    stream << document;
    return stream.str();
}
} // namespace orbit::terrain_geology
