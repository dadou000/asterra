#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/mesh_import/MeshAsset.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace orbit::mesh_sdf
{
// A decoded RGBA8 image a material's texture binding refers to (sRGB for base
// colour and emissive). Pixels are row-major, top row first, `width * height`
// texels of 4 bytes.
struct SdfImage
{
    u32 width{0U};
    u32 height{0U};
    std::vector<std::byte> rgba;
};

struct SdfSettings
{
    // Voxel edge in the mesh's own units (metres). The grid is coarsened if a
    // dimension or the voxel total would exceed the limits below.
    f32 targetVoxelMeters{0.15F};
    u32 maximumDimension{256U};
    u64 maximumVoxels{12'000'000ULL};
    // Empty margin around the mesh bounds, in voxels, so rays starting just
    // outside still sample a valid distance.
    u32 marginVoxels{4U};
    // 0 = hardware concurrency.
    u32 threads{0U};
};

// An unsigned distance field of a triangle mesh in its local frame, plus the
// surface attributes (albedo, normal) of the voxels the surface passes through.
//
// Unsigned on purpose: every ray is traced from a point in air, so the sign is
// never needed, and imported meshes are rarely watertight (curtains, planes).
// Distances are exact (nearest-triangle) at every voxel centre.
struct MeshSdf
{
    std::array<u32, 3> dimensions{};
    // Position of voxel (0,0,0)'s centre in mesh space.
    std::array<f32, 3> origin{};
    f32 voxelSize{0.15F};

    // dimensions.x * y * z entries, x fastest.
    std::vector<f32> distance;
    // RGBA8 linear albedo of the nearest surface point for voxels within
    // `surfaceBand` of the surface (alpha = 255), else 0.
    std::vector<u32> albedo;
    // Octahedral-encoded (two 16-bit snorm) unit normal of that surface point,
    // flipped to face the voxel centre.
    std::vector<u32> normal;
    // Linear emissive RGB (RGBE-free): rgb8 in the low 24 bits, scaled by
    // `emissiveScale`; 0 when the surface does not emit.
    std::vector<u32> emissive;
    f32 emissiveScale{1.0F};

    // Distance below which a voxel counts as "on the surface".
    f32 surfaceBand{0.13F};
    f32 maximumDistance{0.0F};
    u32 surfaceVoxelCount{0U};

    [[nodiscard]] std::size_t VoxelCount() const noexcept
    {
        return static_cast<std::size_t>(dimensions[0]) * dimensions[1] *
               dimensions[2];
    }

    [[nodiscard]] std::size_t Index(u32 x, u32 y, u32 z) const noexcept
    {
        return (static_cast<std::size_t>(z) * dimensions[1] + y) *
                   dimensions[0] +
               x;
    }
};

// Builds the field. `images` is indexed like `asset.images` (entries that
// failed to decode or were never decoded may be empty; those textures are
// treated as white). Safe to run on a worker thread; uses its own threads.
[[nodiscard]] MeshSdf BuildMeshSdf(
    const mesh_import::MeshAsset& asset,
    std::span<const SdfImage> images,
    const SdfSettings& settings = {});

// Octahedral normal packing used by MeshSdf::normal (exposed for the GPU
// decode tests and the debug views).
[[nodiscard]] u32 PackOctahedralNormal(f32 x, f32 y, f32 z) noexcept;
[[nodiscard]] std::array<f32, 3> UnpackOctahedralNormal(u32 packed) noexcept;
} // namespace orbit::mesh_sdf
