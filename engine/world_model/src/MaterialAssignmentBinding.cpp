#include <orbit/world_model/MaterialAssignmentBinding.hpp>

#include <orbit/world_model/WorldSchemas.hpp>

#include <algorithm>
#include <variant>

namespace orbit::world_model
{
namespace
{
template <typename T>
[[nodiscard]] T PropertyOr(
    const scene::ObjectStore& objects,
    const scene::ObjectId object,
    const schema::PropertyId property,
    const T& fallback)
{
    const auto value =
        objects.GetProperty(
            object,
            property);

    if (!value.has_value())
    {
        return fallback;
    }

    if (const auto* typed =
            std::get_if<T>(&*value))
    {
        return *typed;
    }

    return fallback;
}

void Gather(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& record,
    std::vector<ResolvedMaterialAssignment>& result)
{
    if (record.type ==
            kMaterialAssignmentType &&
        record.parent.has_value() &&
        PropertyOr<bool>(
            objects,
            record.id,
            kMaterialAssignmentEnabled,
            true))
    {
        auto asset =
            PropertyOr<std::string>(
                objects,
                record.id,
                kMaterialAssignmentAsset,
                {});

        auto slot =
            PropertyOr<std::string>(
                objects,
                record.id,
                kMaterialAssignmentSlot,
                "default");

        if (slot.empty())
        {
            slot = "default";
        }

        if (!asset.empty())
        {
            result.push_back({
                .assignment = record.id,
                .owner = *record.parent,
                .assetId = std::move(asset),
                .slot = std::move(slot)
            });
        }
    }

    for (const auto& child :
         objects.Children(record.id))
    {
        Gather(
            objects,
            child,
            result);
    }
}
} // namespace

std::vector<ResolvedMaterialAssignment>
ResolveMaterialAssignments(
    const scene::ObjectStore& objects,
    const std::optional<scene::ObjectId> root)
{
    std::vector<ResolvedMaterialAssignment> result;

    if (root.has_value())
    {
        const auto record =
            objects.Find(*root);

        if (record.has_value())
        {
            Gather(
                objects,
                *record,
                result);
        }
    }
    else
    {
        for (const auto& record :
             objects.Roots())
        {
            Gather(
                objects,
                record,
                result);
        }
    }

    std::stable_sort(
        result.begin(),
        result.end(),
        [](const auto& a,
           const auto& b)
        {
            if (a.owner != b.owner)
            {
                return a.owner < b.owner;
            }

            if (a.slot != b.slot)
            {
                return a.slot < b.slot;
            }

            return a.assignment < b.assignment;
        });

    return result;
}
} // namespace orbit::world_model
