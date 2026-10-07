#pragma once

#include <orbit/core/Types.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace orbit::mesh_import
{
// One vertex of an imported mesh, already flattened into the asset's local
// frame (glTF axes: +Y up, right-handed, metres). 48 bytes; the GPU reads
// this layout directly, so do not reorder it.
struct MeshVertex
{
    std::array<f32, 3> position{};
    std::array<f32, 3> normal{0.0F, 1.0F, 0.0F};
    // xyz = tangent, w = bitangent sign (+1 / -1), as glTF defines it.
    std::array<f32, 4> tangent{1.0F, 0.0F, 0.0F, 1.0F};
    std::array<f32, 2> uv{};
};
static_assert(sizeof(MeshVertex) == 48U);

enum class AlphaMode : u8
{
    Opaque,
    Mask,
    Blend
};

// A texture slot of a material. texture < 0 means "no texture".
struct TextureBinding
{
    i32 texture{-1};
    u32 uvSet{0U};
    // Normal map scale or occlusion strength, per the slot.
    f32 scale{1.0F};

    [[nodiscard]] bool Present() const noexcept
    {
        return texture >= 0;
    }
};

struct MeshMaterial
{
    std::string name;
    std::array<f32, 4> baseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};
    f32 metallicFactor{1.0F};
    f32 roughnessFactor{1.0F};
    // Linear emissive radiance factor, already multiplied by
    // KHR_materials_emissive_strength.
    std::array<f32, 3> emissiveFactor{};

    TextureBinding baseColorTexture;
    TextureBinding metallicRoughnessTexture; // G = roughness, B = metallic
    TextureBinding normalTexture;
    TextureBinding occlusionTexture; // R
    TextureBinding emissiveTexture;

    AlphaMode alphaMode{AlphaMode::Opaque};
    f32 alphaCutoff{0.5F};
    bool doubleSided{false};
};

struct MeshTexture
{
    i32 image{-1};
    bool repeatU{true};
    bool repeatV{true};
};

// An encoded image (PNG/JPEG/...) exactly as stored in the source file. The
// importer never decodes pixels: the renderer decodes on its own thread and
// owns colour-space choices (base colour and emissive are sRGB).
struct MeshImage
{
    std::string name;
    std::string mimeType;
    std::vector<std::byte> encoded;
};

// A run of indices drawn with one material.
struct MeshPart
{
    u32 firstIndex{0U};
    u32 indexCount{0U};
    // Index into MeshAsset::materials.
    u32 material{0U};
};

struct MeshAsset
{
    std::string name;
    std::filesystem::path sourcePath;

    std::vector<MeshVertex> vertices;
    std::vector<u32> indices;
    // Sorted by material; one part per material that has geometry.
    std::vector<MeshPart> parts;
    // Never empty after a successful import: a default material is appended
    // when a primitive has none.
    std::vector<MeshMaterial> materials;
    std::vector<MeshTexture> textures;
    std::vector<MeshImage> images;

    std::array<f64, 3> boundsMin{};
    std::array<f64, 3> boundsMax{};

    // Non-fatal problems: ignored extensions, missing tangents that had to be
    // generated, skipped primitives. Empty for a perfectly clean import.
    std::vector<std::string> warnings;

    [[nodiscard]] u32 TriangleCount() const noexcept
    {
        return static_cast<u32>(indices.size() / 3U);
    }

    [[nodiscard]] std::array<f64, 3> Extent() const noexcept
    {
        return {
            boundsMax[0] - boundsMin[0],
            boundsMax[1] - boundsMin[1],
            boundsMax[2] - boundsMin[2]
        };
    }
};
} // namespace orbit::mesh_import
