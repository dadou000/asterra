#include <orbit/terrain_geology/GeologicalMaterial.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_geology
{
namespace
{
[[nodiscard]] bool NormalizedCoefficient(
    const f32 value) noexcept
{
    return
        std::isfinite(value) &&
        value >= 0.0F &&
        value <= 1.0F;
}

[[nodiscard]] bool EqualName(
    const std::string_view left,
    const std::string_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (std::size_t index = 0;
         index < left.size();
         ++index)
    {
        const auto a =
            static_cast<unsigned char>(
                left[index]);
        const auto b =
            static_cast<unsigned char>(
                right[index]);

        if (std::tolower(a) !=
            std::tolower(b))
        {
            return false;
        }
    }

    return true;
}

[[nodiscard]] f32 PositiveForcing(
    const f32 value) noexcept
{
    return std::isfinite(value)
        ? std::max(value, 0.0F)
        : 0.0F;
}
} // namespace

bool GeologicalMaterial::IsValid() const noexcept
{
    return
        id.IsValid() &&
        !name.empty() &&
        NormalizedCoefficient(hardness) &&
        NormalizedCoefficient(cohesion) &&
        NormalizedCoefficient(
            hydraulicErodibility) &&
        NormalizedCoefficient(
            aeolianErodibility) &&
        NormalizedCoefficient(permeability) &&
        NormalizedCoefficient(
            chemicalWeatherability) &&
        NormalizedCoefficient(
            fractureTendency) &&
        std::isfinite(density) &&
        density > 0.0F;
}

GpuGeologicalMaterial ToGpuMaterial(
    const GeologicalMaterial& material) noexcept
{
    return {
        .hardness = material.hardness,
        .cohesion = material.cohesion,
        .hydraulicErodibility =
            material.hydraulicErodibility,
        .aeolianErodibility =
            material.aeolianErodibility,
        .permeability = material.permeability,
        .chemicalWeatherability =
            material.chemicalWeatherability,
        .fractureTendency =
            material.fractureTendency,
        .density = material.density
    };
}

std::optional<u32>
GeologicalMaterialGpuTable::IndexOf(
    const RockTypeId id) const noexcept
{
    const auto found =
        std::find(
            rockTypes.begin(),
            rockTypes.end(),
            id);

    if (found == rockTypes.end())
    {
        return std::nullopt;
    }

    return static_cast<u32>(
        std::distance(
            rockTypes.begin(),
            found));
}

void GeologicalMaterialLibrary::Upsert(
    GeologicalMaterial material)
{
    if (!material.IsValid())
    {
        throw std::invalid_argument(
            "Orbit geological material is invalid. IDs/names must be present, normalized coefficients must be in [0, 1], and density must be positive.");
    }

    const auto found =
        index_.find(
            material.id);

    if (found == index_.end())
    {
        index_.emplace(
            material.id,
            materials_.size());

        materials_.push_back(
            std::move(material));
    }
    else
    {
        materials_[found->second] =
            std::move(material);
    }

    ++revision_;
}

bool GeologicalMaterialLibrary::Erase(
    const RockTypeId id)
{
    const auto found =
        index_.find(id);

    if (found == index_.end())
    {
        return false;
    }

    materials_.erase(
        materials_.begin() +
        static_cast<std::ptrdiff_t>(
            found->second));

    RebuildIndex();
    ++revision_;
    return true;
}

const GeologicalMaterial*
GeologicalMaterialLibrary::Find(
    const RockTypeId id) const noexcept
{
    const auto found =
        index_.find(id);

    return found == index_.end()
        ? nullptr
        : &materials_[found->second];
}

const GeologicalMaterial*
GeologicalMaterialLibrary::FindByName(
    const std::string_view name) const noexcept
{
    const auto found =
        std::find_if(
            materials_.begin(),
            materials_.end(),
            [name](
                const GeologicalMaterial& material)
            {
                return EqualName(
                    material.name,
                    name);
            });

    return found == materials_.end()
        ? nullptr
        : &*found;
}

std::span<const GeologicalMaterial>
GeologicalMaterialLibrary::Materials()
    const noexcept
{
    return materials_;
}

std::size_t GeologicalMaterialLibrary::Size()
    const noexcept
{
    return materials_.size();
}

bool GeologicalMaterialLibrary::Empty()
    const noexcept
{
    return materials_.empty();
}

u64 GeologicalMaterialLibrary::Revision()
    const noexcept
{
    return revision_;
}

GeologicalMaterialGpuTable
GeologicalMaterialLibrary::BuildGpuTable() const
{
    GeologicalMaterialGpuTable table{};
    table.rockTypes.reserve(
        materials_.size());
    table.materials.reserve(
        materials_.size());

    for (const GeologicalMaterial& material :
         materials_)
    {
        table.rockTypes.push_back(
            material.id);
        table.materials.push_back(
            ToGpuMaterial(material));
    }

    return table;
}

void GeologicalMaterialLibrary::RebuildIndex()
{
    index_.clear();
    index_.reserve(
        materials_.size());

    for (std::size_t index = 0;
         index < materials_.size();
         ++index)
    {
        index_.emplace(
            materials_[index].id,
            index);
    }
}

f32 GeologicalErosionResponse::Total()
    const noexcept
{
    return
        hydraulicDetachment +
        aeolianDetachment +
        chemicalWeathering +
        fracturePotential;
}

GeologicalErosionResponse
EvaluateIntrinsicErosionResponse(
    const GeologicalMaterial& material,
    const GeologicalErosionForcing& forcing) noexcept
{
    if (!material.IsValid())
    {
        return {};
    }

    return {
        .hydraulicDetachment =
            PositiveForcing(forcing.hydraulic) *
            material.hydraulicErodibility,
        .aeolianDetachment =
            PositiveForcing(forcing.aeolian) *
            material.aeolianErodibility,
        .chemicalWeathering =
            PositiveForcing(forcing.chemical) *
            material.chemicalWeatherability,
        .fracturePotential =
            PositiveForcing(forcing.fracture) *
            material.fractureTendency
    };
}
} // namespace orbit::terrain_geology
