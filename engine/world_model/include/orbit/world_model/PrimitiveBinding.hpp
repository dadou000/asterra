#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <optional>
#include <string>
#include <vector>

namespace orbit::world_model
{
// Values match the stored kPrimitiveShape integer.
enum class PrimitiveShape : u8
{
    Box = 0,
    Sphere = 1,
    Cylinder = 2,
    Capsule = 3,
    Plane = 4
};

// Runtime view of one Primitive object. sizeMeters is the full bounding
// dimensions in the primitive's local axes: a sphere is an ellipsoid inscribed
// in the box, a cylinder/capsule runs along local +Y, and a plane is a thin
// quad in the local XZ plane (sizeMeters.y is ignored).
struct ResolvedPrimitive
{
    scene::ObjectId object{};
    PrimitiveShape shape{PrimitiveShape::Box};

    // Relative to the parent object's frame.
    math::Double3 positionMeters{};
    math::Double3 eulerDegrees{};
    math::Double3 sizeMeters{1.0, 1.0, 1.0};

    std::string materialAsset;
    bool castShadows{true};
};

// Gathers enabled, well-formed primitives under root (or the whole world when
// root is empty). Disabled primitives and primitives with non-finite or
// non-positive size are skipped, never reported as partial data.
[[nodiscard]] std::vector<ResolvedPrimitive> ResolvePrimitives(
    const scene::ObjectStore& objects,
    std::optional<scene::ObjectId> root = std::nullopt);
} // namespace orbit::world_model
