#pragma once

#include <orbit/scene/ObjectStore.hpp>

#include <optional>
#include <string>
#include <vector>

namespace orbit::world_model
{
struct ResolvedMaterialAssignment
{
    scene::ObjectId assignment{};
    scene::ObjectId owner{};
    std::string assetId;
    std::string slot{"default"};
};

[[nodiscard]] std::vector<ResolvedMaterialAssignment>
ResolveMaterialAssignments(
    const scene::ObjectStore& objects,
    std::optional<scene::ObjectId> root = std::nullopt);
} // namespace orbit::world_model
