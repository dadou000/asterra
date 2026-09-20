#include <orbit/terrain_geology/GeologicalMaterial.hpp>

#include "ReferenceGeologicalMaterials.hpp"

#include <stdexcept>
#include <utility>

namespace orbit::terrain_geology
{
GeologicalMaterialLibrary
LoadReferenceGeologicalMaterialLibrary()
{
    GeologicalMaterialLibrary result;

    for (const std::string_view text :
         detail::kReferenceGeologicalMaterialToml)
    {
        auto material =
            ParseGeologicalMaterialToml(
                text);

        if (result.Find(material.id) != nullptr ||
            result.FindByName(material.name) != nullptr)
        {
            throw std::logic_error(
                "Embedded reference geology contains a duplicate material.");
        }

        result.Upsert(
            std::move(material));
    }

    return result;
}
} // namespace orbit::terrain_geology
