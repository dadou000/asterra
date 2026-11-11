#include <orbit/mesh_import/PrimitiveMesh.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

using namespace orbit;
using namespace orbit::mesh_import;

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

// Signed volume by the divergence theorem: positive when the winding points
// outward everywhere.
double SignedVolume(const MeshAsset& asset)
{
    double volume = 0.0;
    for (std::size_t i = 0; i + 2 < asset.indices.size(); i += 3)
    {
        const auto& a = asset.vertices[asset.indices[i]].position;
        const auto& b = asset.vertices[asset.indices[i + 1]].position;
        const auto& c = asset.vertices[asset.indices[i + 2]].position;
        volume += (a[0] * (b[1] * c[2] - b[2] * c[1]) -
                   a[1] * (b[0] * c[2] - b[2] * c[0]) +
                   a[2] * (b[0] * c[1] - b[1] * c[0])) /
                  6.0;
    }
    return volume;
}

bool Near(const double a, const double b, const double relative)
{
    return std::abs(a - b) <= relative * std::max(std::abs(b), 1.0e-9);
}

void CheckWellFormed(const MeshAsset& asset)
{
    CHECK(!asset.vertices.empty());
    CHECK(asset.indices.size() % 3U == 0U);
    CHECK(asset.parts.size() == 1U);
    CHECK(asset.materials.size() == 1U);
    CHECK(asset.parts[0].indexCount == asset.indices.size());
    for (const auto index : asset.indices)
    {
        CHECK(index < asset.vertices.size());
    }
    for (const auto& vertex : asset.vertices)
    {
        const double length = std::sqrt(
            vertex.normal[0] * vertex.normal[0] +
            vertex.normal[1] * vertex.normal[1] +
            vertex.normal[2] * vertex.normal[2]);
        CHECK(std::abs(length - 1.0) < 1.0e-4);
    }
}

void TestBox()
{
    const auto mesh = BuildPrimitiveMesh(
        PrimitiveMeshShape::Box, {2.0, 4.0, 6.0}, {});
    CheckWellFormed(mesh);
    CHECK(mesh.TriangleCount() == 12U);
    CHECK(Near(SignedVolume(mesh), 48.0, 1.0e-5));
    CHECK(Near(mesh.boundsMax[0], 1.0, 1.0e-6));
    CHECK(Near(mesh.boundsMin[1], -2.0, 1.0e-6));
    CHECK(Near(mesh.boundsMax[2], 3.0, 1.0e-6));
}

void TestSphereIsAnEllipsoid()
{
    const auto mesh = BuildPrimitiveMesh(
        PrimitiveMeshShape::Sphere, {2.0, 4.0, 6.0}, {});
    CheckWellFormed(mesh);
    const double exact = 4.0 / 3.0 * std::numbers::pi * 1.0 * 2.0 * 3.0;
    // A tessellated sphere is slightly smaller than the true one.
    CHECK(SignedVolume(mesh) > 0.0);
    CHECK(Near(SignedVolume(mesh), exact, 0.02));
    CHECK(Near(mesh.boundsMax[1], 2.0, 1.0e-4));
}

void TestCylinder()
{
    const auto mesh = BuildPrimitiveMesh(
        PrimitiveMeshShape::Cylinder, {2.0, 3.0, 2.0}, {});
    CheckWellFormed(mesh);
    CHECK(Near(SignedVolume(mesh), std::numbers::pi * 1.0 * 3.0, 0.01));
    CHECK(Near(mesh.boundsMax[1], 1.5, 1.0e-5));
}

void TestCapsule()
{
    const auto mesh = BuildPrimitiveMesh(
        PrimitiveMeshShape::Capsule, {1.0, 3.0, 2.0}, {});
    CheckWellFormed(mesh);
    // Radius is half the smaller of X and Z; the height is the full Y size.
    const double r = 0.5;
    const double exact = std::numbers::pi * r * r * (3.0 - 2.0 * r) +
                         4.0 / 3.0 * std::numbers::pi * r * r * r;
    CHECK(Near(SignedVolume(mesh), exact, 0.02));
    CHECK(Near(mesh.boundsMax[1], 1.5, 1.0e-4));
    CHECK(Near(mesh.boundsMax[0], 0.5, 1.0e-4));
}

void TestCapsuleShorterThanItsDiameterBecomesASphere()
{
    const auto mesh = BuildPrimitiveMesh(
        PrimitiveMeshShape::Capsule, {2.0, 0.5, 2.0}, {});
    CheckWellFormed(mesh);
    CHECK(Near(mesh.boundsMax[1], 1.0, 1.0e-4));
}

void TestPlane()
{
    const auto mesh = BuildPrimitiveMesh(
        PrimitiveMeshShape::Plane, {4.0, 9.0, 2.0}, {});
    CheckWellFormed(mesh);
    CHECK(mesh.TriangleCount() == 2U);
    CHECK(Near(mesh.boundsMax[0], 2.0, 1.0e-6));
    CHECK(Near(mesh.boundsMax[2], 1.0, 1.0e-6));
    CHECK(mesh.boundsMin[1] == 0.0 && mesh.boundsMax[1] == 0.0);
}

void TestInvalidSizesProduceNothing()
{
    for (const double bad : {0.0, -1.0, std::nan(""),
                              std::numeric_limits<double>::infinity()})
    {
        const auto mesh = BuildPrimitiveMesh(
            PrimitiveMeshShape::Box, {1.0, bad, 1.0}, {});
        CHECK(mesh.vertices.empty());
        CHECK(mesh.indices.empty());
        CHECK(mesh.parts.empty());
    }
}

void TestMaterialIsCarried()
{
    MeshMaterial material;
    material.metallicFactor = 1.0F;
    material.roughnessFactor = 0.02F;
    const auto mesh = BuildPrimitiveMesh(
        PrimitiveMeshShape::Sphere, {1.0, 1.0, 1.0}, material);
    CHECK(mesh.materials.size() == 1U);
    CHECK(mesh.materials[0].roughnessFactor == 0.02F);
}
} // namespace

int main()
{
    TestBox();
    TestSphereIsAnEllipsoid();
    TestCylinder();
    TestCapsule();
    TestCapsuleShorterThanItsDiameterBecomesASphere();
    TestPlane();
    TestInvalidSizesProduceNothing();
    TestMaterialIsCarried();
    return failures == 0 ? 0 : 1;
}
