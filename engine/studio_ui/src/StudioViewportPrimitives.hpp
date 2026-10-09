#pragma once

#include <orbit/mesh_import/MeshAsset.hpp>
#include <orbit/world_model/PrimitiveBinding.hpp>

#include <functional>
#include <string>

namespace orbit::studio_ui
{
// What the viewport asks the mesh library for to draw one Primitive object.
// The key encodes every input of `build` (shape, size and, for opaque
// surfaces, the material), so editing a property just selects another
// generated model and the old one is released by the library after its idle
// time. Glass uses the geometry only: its optics live in the glass pass.
struct PrimitiveModelRequest
{
    std::string key;
    std::function<mesh_import::MeshAsset()> build;
};

// Roughness a Mirror primitive is built with. The mesh surface shader clamps
// roughness to 0.045, so this is the smoothest the pipeline can show.
inline constexpr float kMirrorRoughness = 0.0F;

[[nodiscard]] PrimitiveModelRequest MakePrimitiveModelRequest(
    const world_model::ResolvedPrimitive& primitive);

// The opaque material for Standard and Mirror surfaces (glass returns the
// neutral default; it is not rasterised into the surface buffer).
[[nodiscard]] mesh_import::MeshMaterial PrimitiveSurfaceMaterial(
    const world_model::ResolvedPrimitive& primitive);
} // namespace orbit::studio_ui
