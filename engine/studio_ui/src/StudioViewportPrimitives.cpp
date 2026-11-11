#include "StudioViewportPrimitives.hpp"

#include <orbit/mesh_import/PrimitiveMesh.hpp>
#include <orbit/post_process/HumanEyeAdaptation.hpp>

#include <format>

namespace orbit::studio_ui
{
mesh_import::MeshMaterial PrimitiveSurfaceMaterial(
    const world_model::ResolvedPrimitive& primitive)
{
    mesh_import::MeshMaterial material;
    material.name = "primitive";
    material.doubleSided = true;
    const auto f = [](const f64 value) { return static_cast<f32>(value); };

    switch (primitive.surface)
    {
    case world_model::PrimitiveSurface::Mirror:
        // A perfect reflector: fully metallic, no diffuse term, smoothest
        // roughness the surface shader allows.
        material.baseColorFactor = {
            f(primitive.color.x), f(primitive.color.y), f(primitive.color.z),
            1.0F};
        material.metallicFactor = 1.0F;
        material.roughnessFactor = kMirrorRoughness;
        break;
    case world_model::PrimitiveSurface::Glass:
        break;
    case world_model::PrimitiveSurface::Emissive:
    {
        // The surface itself barely reflects; it radiates `color` at the
        // authored luminance. Scene colour is linear radiance in units of
        // kSceneLuminanceNitsPerUnit nits, so that is also what the emissive
        // factor (and the distance field's emissive voxels) hold.
        const f32 radiance = static_cast<f32>(
            primitive.emissionNits /
            static_cast<f64>(post_process::kSceneLuminanceNitsPerUnit));
        material.baseColorFactor = {
            f(primitive.color.x) * 0.05F,
            f(primitive.color.y) * 0.05F,
            f(primitive.color.z) * 0.05F,
            1.0F};
        material.metallicFactor = 0.0F;
        material.roughnessFactor = 0.6F;
        material.emissiveFactor = {
            f(primitive.color.x) * radiance,
            f(primitive.color.y) * radiance,
            f(primitive.color.z) * radiance};
        // Lit analytically by the emissive-lights pass, not by GI rays.
        material.emissiveInGi = false;
        break;
    }
    case world_model::PrimitiveSurface::Standard:
        material.baseColorFactor = {
            f(primitive.color.x), f(primitive.color.y), f(primitive.color.z),
            1.0F};
        material.metallicFactor = f(primitive.metallic);
        material.roughnessFactor = f(primitive.roughness);
        break;
    }
    return material;
}

PrimitiveModelRequest MakePrimitiveModelRequest(
    const world_model::ResolvedPrimitive& primitive)
{
    const auto shape =
        static_cast<mesh_import::PrimitiveMeshShape>(primitive.shape);
    const std::array<f64, 3> size{
        primitive.sizeMeters.x,
        primitive.sizeMeters.y,
        primitive.sizeMeters.z};
    const mesh_import::MeshMaterial material =
        PrimitiveSurfaceMaterial(primitive);

    std::string key = std::format(
        "generated/primitive/{}/{:.6g}x{:.6g}x{:.6g}",
        static_cast<int>(primitive.shape), size[0], size[1], size[2]);
    if (primitive.surface != world_model::PrimitiveSurface::Glass)
    {
        key += std::format(
            "/s{}/c{:.4g},{:.4g},{:.4g}/r{:.4g}/m{:.4g}/e{:.4g},{:.4g},{:.4g}",
            static_cast<int>(primitive.surface),
            material.baseColorFactor[0],
            material.baseColorFactor[1],
            material.baseColorFactor[2],
            material.roughnessFactor,
            material.metallicFactor,
            material.emissiveFactor[0],
            material.emissiveFactor[1],
            material.emissiveFactor[2]);
    }
    else
    {
        key += "/glass";
    }

    return {
        .key = std::move(key),
        .build = [shape, size, material]
        {
            return mesh_import::BuildPrimitiveMesh(shape, size, material);
        }};
}
} // namespace orbit::studio_ui
