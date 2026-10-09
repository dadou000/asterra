#pragma once

#include <orbit/commands/CommandService.hpp>
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

// How the primitive's surface behaves. Values match kPrimitiveSurface.
//   Standard: opaque, Color / Roughness / Metallic.
//   Mirror:   opaque, metallic, near-zero roughness (perfect reflector).
//   Glass:    refracting dielectric, Color is the transmission tint; it also
//             focuses sunlight into caustics on nearby surfaces.
//   Emissive: a light-emitting surface: Color is the emitted hue and
//             emissionNits its luminance (cd/m^2). Lights its surroundings
//             through the distance-field GI like any emissive mesh.
enum class PrimitiveSurface : u8
{
    Standard = 0,
    Mirror = 1,
    Glass = 2,
    Emissive = 3
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

    PrimitiveSurface surface{PrimitiveSurface::Standard};
    math::Double3 color{0.8, 0.8, 0.8};
    f64 roughness{0.5};
    f64 metallic{0.0};
    f64 indexOfRefraction{1.5};
    bool caustics{true};
    // Luminance of an Emissive surface in cd/m^2 (nits); ignored otherwise.
    f64 emissionNits{100000.0};

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

namespace orbit::world_model
{
// Everything needed to author one Primitive in a single undo step.
struct PrimitiveCreateRequest
{
    std::string name;
    PrimitiveShape shape{PrimitiveShape::Sphere};
    PrimitiveSurface surface{PrimitiveSurface::Standard};
    // Relative to the parent object's frame.
    math::Double3 positionMeters{};
    math::Double3 eulerDegrees{};
    math::Double3 sizeMeters{1.0, 1.0, 1.0};
    // Standard: albedo. Glass: transmittance per metre. Mirror: reflectance tint.
    math::Double3 color{0.8, 0.8, 0.8};
    f64 roughness{0.5};
    f64 metallic{0.0};
    f64 indexOfRefraction{1.5};
    bool caustics{true};
    f64 emissionNits{100000.0};
};

// Defaults that make each surface look right out of the box: a Mirror is
// white, a Glass is clear (white tint, IOR 1.5) with a smooth finish, an
// Emissive is a warm white 100,000 nit emitter.
[[nodiscard]] PrimitiveCreateRequest MakePrimitivePreset(
    PrimitiveShape shape,
    PrimitiveSurface surface);

// Creates the object under `parent` and sets every property inside one
// command-service transaction; a failure rolls the whole thing back. Returns
// the new object's id.
[[nodiscard]] scene::ObjectId CreatePrimitive(
    commands::CommandService& commands,
    scene::ObjectId parent,
    const PrimitiveCreateRequest& request);
} // namespace orbit::world_model
