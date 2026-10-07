#include <orbit/mesh_sdf/MeshSdf.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
int failures = 0;

#define CHECK(condition)                                                      \
    do                                                                        \
    {                                                                         \
        if (!(condition))                                                     \
        {                                                                     \
            std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__,        \
                         __LINE__, #condition);                               \
            ++failures;                                                       \
        }                                                                     \
    } while (false)

using namespace orbit;

// A 2 m x 2 m quad in the z = 0 plane facing +Z, red, with a thickness-free
// surface.
mesh_import::MeshAsset MakeQuad(const f32 size = 2.0F)
{
    mesh_import::MeshAsset asset;
    const f32 corners[4][3] = {
        {0, 0, 0}, {size, 0, 0}, {size, size, 0}, {0, size, 0}};
    for (const auto& c : corners)
    {
        mesh_import::MeshVertex vertex;
        vertex.position = {c[0], c[1], c[2]};
        vertex.normal = {0.0F, 0.0F, 1.0F};
        vertex.uv = {0.0F, 0.0F};
        asset.vertices.push_back(vertex);
    }
    asset.indices = {0, 1, 2, 0, 2, 3};
    mesh_import::MeshMaterial material;
    material.baseColorFactor = {1.0F, 0.0F, 0.0F, 1.0F};
    asset.materials.push_back(material);
    asset.parts.push_back({.firstIndex = 0U, .indexCount = 6U, .material = 0U});
    asset.boundsMin = {0.0, 0.0, 0.0};
    asset.boundsMax = {size, size, 0.0};
    return asset;
}

bool Near(const f32 a, const f32 b, const f32 tolerance = 1.0e-3F)
{
    return std::abs(a - b) <= tolerance;
}
} // namespace

int main()
{
    // Distance above and below a flat quad equals |z|.
    {
        mesh_sdf::SdfSettings settings;
        settings.targetVoxelMeters = 0.25F;
        settings.marginVoxels = 4U;
        settings.threads = 2U;
        const auto sdf = mesh_sdf::BuildMeshSdf(MakeQuad(), {}, settings);
        CHECK(sdf.VoxelCount() > 0U);
        CHECK(Near(sdf.voxelSize, 0.25F));

        // Voxel centred over the middle of the quad, a few voxels up.
        const u32 mx = static_cast<u32>(std::lround((1.0F - sdf.origin[0]) / sdf.voxelSize));
        const u32 my = static_cast<u32>(std::lround((1.0F - sdf.origin[1]) / sdf.voxelSize));
        for (const i32 dz : {-3, -1, 0, 1, 2, 4})
        {
            const u32 mz = static_cast<u32>(
                std::lround((0.0F - sdf.origin[2]) / sdf.voxelSize)) +
                static_cast<u32>(dz + 8) - 8U;
            const f32 z = sdf.origin[2] + static_cast<f32>(mz) * sdf.voxelSize;
            CHECK(Near(sdf.distance[sdf.Index(mx, my, mz)], std::abs(z)));
        }

        // Surface voxels carry the red albedo; far voxels carry nothing.
        const u32 sz = static_cast<u32>(std::lround((0.0F - sdf.origin[2]) / sdf.voxelSize));
        const u32 surface = sdf.albedo[sdf.Index(mx, my, sz)];
        CHECK((surface & 0xFFU) == 255U);          // red
        CHECK(((surface >> 8U) & 0xFFU) == 0U);    // green
        CHECK(((surface >> 24U) & 0xFFU) == 255U); // on surface
        CHECK(sdf.albedo[sdf.Index(mx, my, sz + 4U)] == 0U);

        // The stored normal of a surface voxel is the surface's.
        const auto flat = mesh_sdf::UnpackOctahedralNormal(
            sdf.normal[sdf.Index(mx, my, sz)]);
        CHECK(std::abs(flat[2]) > 0.99F);
        CHECK(sdf.surfaceVoxelCount > 0U);
    }

    // On a slanted quad both sides of the surface are within the surface band,
    // and the stored normal is flipped to face the voxel it belongs to.
    {
        mesh_import::MeshAsset asset = MakeQuad();
        const f32 corners[4][3] = {{0, 0, 0}, {2, 0, 0}, {2, 2, 2}, {0, 2, 2}};
        const f32 inverseRoot2 = 0.70710678F;
        for (std::size_t i = 0U; i < 4U; ++i)
        {
            asset.vertices[i].position = {corners[i][0], corners[i][1], corners[i][2]};
            asset.vertices[i].normal = {0.0F, -inverseRoot2, inverseRoot2};
        }
        asset.boundsMax = {2.0, 2.0, 2.0};

        mesh_sdf::SdfSettings settings;
        settings.targetVoxelMeters = 0.25F;
        settings.threads = 2U;
        const auto sdf = mesh_sdf::BuildMeshSdf(asset, {}, settings);

        // A voxel exactly on the plane y == z, in the middle of the quad.
        const auto coordinate = [&](const f32 value, const std::size_t axis)
        {
            return static_cast<u32>(
                std::lround((value - sdf.origin[axis]) / sdf.voxelSize));
        };
        const u32 x = coordinate(1.0F, 0U);
        const u32 y = coordinate(1.0F, 1U);
        const u32 z = coordinate(1.0F, 2U);

        const auto onPlane = mesh_sdf::UnpackOctahedralNormal(
            sdf.normal[sdf.Index(x, y, z)]);
        const auto above = mesh_sdf::UnpackOctahedralNormal(
            sdf.normal[sdf.Index(x, y, z + 1U)]);
        const auto below = mesh_sdf::UnpackOctahedralNormal(
            sdf.normal[sdf.Index(x, y, z - 1U)]);

        // z + 1 is on the +n side (n = (0,-1,1)/sqrt 2), z - 1 on the -n side.
        const auto along = [&](const std::array<f32, 3>& v)
        {
            return -v[1] * inverseRoot2 + v[2] * inverseRoot2;
        };
        CHECK(Near(sdf.distance[sdf.Index(x, y, z + 1U)], 0.25F * inverseRoot2));
        CHECK(Near(sdf.distance[sdf.Index(x, y, z - 1U)], 0.25F * inverseRoot2));
        CHECK(along(onPlane) > 0.99F);
        CHECK(along(above) > 0.99F);
        CHECK(along(below) < -0.99F);
    }

    // Octahedral normals round-trip, including the lower hemisphere.
    {
        const f32 samples[][3] = {
            {0.0F, 0.0F, 1.0F},  {0.0F, 0.0F, -1.0F}, {1.0F, 0.0F, 0.0F},
            {0.0F, -1.0F, 0.0F}, {0.577F, 0.577F, 0.577F},
            {-0.5F, 0.3F, -0.81F}};
        for (const auto& s : samples)
        {
            const f32 length = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
            const auto out = mesh_sdf::UnpackOctahedralNormal(
                mesh_sdf::PackOctahedralNormal(
                    s[0] / length, s[1] / length, s[2] / length));
            CHECK(Near(out[0], s[0] / length, 2.0e-3F));
            CHECK(Near(out[1], s[1] / length, 2.0e-3F));
            CHECK(Near(out[2], s[2] / length, 2.0e-3F));
        }
    }

    // A huge mesh is coarsened to the dimension limit.
    {
        mesh_sdf::SdfSettings settings;
        settings.targetVoxelMeters = 0.05F;
        settings.maximumDimension = 64U;
        settings.maximumVoxels = 300'000ULL;
        settings.threads = 2U;
        const auto sdf = mesh_sdf::BuildMeshSdf(MakeQuad(40.0F), {}, settings);
        CHECK(sdf.dimensions[0] <= 64U && sdf.dimensions[1] <= 64U &&
              sdf.dimensions[2] <= 64U);
        CHECK(sdf.voxelSize > 0.05F);
    }

    // Optional: a real model from the environment.
    return failures;
}
