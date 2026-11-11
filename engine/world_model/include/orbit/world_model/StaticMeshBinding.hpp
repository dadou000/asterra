#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>
#include <string>
#include <vector>

namespace orbit::world_model
{
// Runtime view of one Static Mesh object.
struct ResolvedStaticMesh
{
    scene::ObjectId object{};

    // Project-relative path of the glTF/GLB source (forward slashes).
    std::string meshAsset;

    // Relative to the parent object's frame.
    math::Double3 positionMeters{};
    math::Double3 eulerDegrees{};
    f64 uniformScale{1.0};

    bool castShadows{true};
};

// Gathers enabled, well-formed static meshes under root (or the whole world
// when root is empty). Disabled meshes, meshes without an asset path and
// meshes with a non-finite transform or non-positive scale are skipped,
// never reported as partial data.
[[nodiscard]] std::vector<ResolvedStaticMesh> ResolveStaticMeshes(
    const scene::ObjectStore& objects,
    std::optional<scene::ObjectId> root = std::nullopt);
} // namespace orbit::world_model
