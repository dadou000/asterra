#pragma once

#include <orbit/mesh_import/MeshAsset.hpp>

#include <array>

namespace orbit::mesh_import
{
// Shape ids match world_model::PrimitiveShape (0 Box ... 4 Plane).
enum class PrimitiveMeshShape : u8
{
    Box = 0,
    Sphere = 1,
    Cylinder = 2,
    Capsule = 3,
    Plane = 4
};

// Builds a one-material triangle mesh for a primitive at its real size (metres,
// full bounding dimensions in local axes): the instance transform of a mesh is
// uniform-scale only, so non-uniform sizes are baked into the vertices.
//
// - Box: 24 vertices, flat faces.
// - Sphere: an ellipsoid inscribed in the box, smooth normals.
// - Cylinder / Capsule: run along local +Y. A capsule's radius is half the
//   smaller of X and Z, and its height is clamped to at least two radii.
// - Plane: a quad in local XZ (size.y ignored), normal +Y.
//
// The mesh is centred on the origin, counter-clockwise outward winding, with
// tangents and UVs. `material` is stored as the only entry of
// MeshAsset::materials. Non-finite or non-positive sizes yield an empty asset
// (no vertices, no parts) rather than partial geometry.
[[nodiscard]] MeshAsset BuildPrimitiveMesh(
    PrimitiveMeshShape shape,
    const std::array<f64, 3>& sizeMeters,
    const MeshMaterial& material);
} // namespace orbit::mesh_import
