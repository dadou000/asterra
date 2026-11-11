#include <orbit/mesh_import/PrimitiveMesh.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace orbit::mesh_import
{
namespace
{
constexpr u32 kSegments = 64U;
constexpr u32 kSphereRings = 32U;
constexpr u32 kCapRings = 12U;

using Vec3 = std::array<f32, 3>;

[[nodiscard]] Vec3 Normalized(const Vec3& v) noexcept
{
    const f32 length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1.0e-20F))
    {
        return {0.0F, 1.0F, 0.0F};
    }
    return {v[0] / length, v[1] / length, v[2] / length};
}

class Builder
{
public:
    u32 Add(
        const Vec3& position,
        const Vec3& normal,
        const Vec3& tangent,
        const std::array<f32, 2>& uv)
    {
        MeshVertex vertex;
        vertex.position = position;
        vertex.normal = normal;
        vertex.tangent = {tangent[0], tangent[1], tangent[2], 1.0F};
        vertex.uv = uv;
        asset.vertices.push_back(vertex);
        return static_cast<u32>(asset.vertices.size() - 1U);
    }

    // Appends a triangle wound so its geometric normal agrees with the vertex
    // normals; the surface builders then do not have to track handedness.
    void Triangle(const u32 a, const u32 b, const u32 c)
    {
        const auto& va = asset.vertices[a];
        const auto& vb = asset.vertices[b];
        const auto& vc = asset.vertices[c];
        const Vec3 e1{
            vb.position[0] - va.position[0],
            vb.position[1] - va.position[1],
            vb.position[2] - va.position[2]};
        const Vec3 e2{
            vc.position[0] - va.position[0],
            vc.position[1] - va.position[1],
            vc.position[2] - va.position[2]};
        const Vec3 geometric{
            e1[1] * e2[2] - e1[2] * e2[1],
            e1[2] * e2[0] - e1[0] * e2[2],
            e1[0] * e2[1] - e1[1] * e2[0]};
        const f32 area2 = geometric[0] * geometric[0] +
                          geometric[1] * geometric[1] +
                          geometric[2] * geometric[2];
        if (!(area2 > 1.0e-18F))
        {
            return; // Degenerate (pole or merged rim): nothing to draw.
        }
        const f32 agreement =
            geometric[0] * (va.normal[0] + vb.normal[0] + vc.normal[0]) +
            geometric[1] * (va.normal[1] + vb.normal[1] + vc.normal[1]) +
            geometric[2] * (va.normal[2] + vb.normal[2] + vc.normal[2]);
        if (agreement >= 0.0F)
        {
            asset.indices.insert(asset.indices.end(), {a, b, c});
        }
        else
        {
            asset.indices.insert(asset.indices.end(), {a, c, b});
        }
    }

    MeshAsset asset;
};

// One point of a surface-of-revolution profile: radius from the Y axis and
// height, plus the outward normal's radial and vertical parts.
struct ProfilePoint
{
    f32 radius;
    f32 height;
    f32 normalRadial;
    f32 normalY;
};

// Revolves `profile` (top to bottom) around +Y. Positions are
// (ax * r * cos, ay * y, az * r * sin); normals follow the inverse-transpose of
// that scale so ellipsoids and elliptic cylinders shade correctly.
void Revolve(
    Builder& builder,
    const std::vector<ProfilePoint>& profile,
    const f32 ax,
    const f32 ay,
    const f32 az)
{
    if (profile.size() < 2U)
    {
        return;
    }

    std::vector<std::vector<u32>> rings;
    f32 arc = 0.0F;
    std::vector<f32> arcAt{0.0F};
    for (std::size_t i = 1U; i < profile.size(); ++i)
    {
        arc += std::hypot(
            profile[i].radius - profile[i - 1U].radius,
            profile[i].height - profile[i - 1U].height);
        arcAt.push_back(arc);
    }
    const f32 arcTotal = std::max(arc, 1.0e-6F);

    for (std::size_t i = 0U; i < profile.size(); ++i)
    {
        std::vector<u32> ring;
        for (u32 j = 0U; j <= kSegments; ++j)
        {
            const f32 angle = 2.0F * std::numbers::pi_v<f32> *
                              static_cast<f32>(j) /
                              static_cast<f32>(kSegments);
            const f32 c = std::cos(angle);
            const f32 s = std::sin(angle);
            const auto& p = profile[i];
            ring.push_back(builder.Add(
                {ax * p.radius * c, ay * p.height, az * p.radius * s},
                Normalized({p.normalRadial * c / ax,
                            p.normalY / ay,
                            p.normalRadial * s / az}),
                Normalized({-s * ax, 0.0F, c * az}),
                {static_cast<f32>(j) / static_cast<f32>(kSegments),
                 arcAt[i] / arcTotal}));
        }
        rings.push_back(std::move(ring));
    }

    for (std::size_t i = 0U; i + 1U < rings.size(); ++i)
    {
        for (u32 j = 0U; j < kSegments; ++j)
        {
            const u32 a = rings[i][j];
            const u32 b = rings[i][j + 1U];
            const u32 c = rings[i + 1U][j];
            const u32 d = rings[i + 1U][j + 1U];
            builder.Triangle(a, c, b);
            builder.Triangle(b, c, d);
        }
    }
}

void BuildBox(Builder& builder, const Vec3& half)
{
    struct Face
    {
        Vec3 normal;
        Vec3 u;
        Vec3 v;
    };
    static constexpr std::array<Face, 6> faces{{
        {{1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}},
        {{-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}},
        {{0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}},
        {{0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},
        {{0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},
        {{0.0F, 0.0F, -1.0F}, {-1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},
    }};

    for (const Face& face : faces)
    {
        std::array<u32, 4> corner{};
        std::size_t index = 0U;
        for (const f32 cv : {-1.0F, 1.0F})
        {
            for (const f32 cu : {-1.0F, 1.0F})
            {
                const Vec3 position{
                    half[0] * (face.normal[0] + cu * face.u[0] + cv * face.v[0]),
                    half[1] * (face.normal[1] + cu * face.u[1] + cv * face.v[1]),
                    half[2] * (face.normal[2] + cu * face.u[2] + cv * face.v[2])};
                corner[index++] = builder.Add(
                    position,
                    face.normal,
                    face.u,
                    {0.5F + 0.5F * cu, 0.5F - 0.5F * cv});
            }
        }
        builder.Triangle(corner[0], corner[1], corner[2]);
        builder.Triangle(corner[1], corner[3], corner[2]);
    }
}

void BuildPlane(Builder& builder, const Vec3& half)
{
    std::array<u32, 4> corner{};
    std::size_t index = 0U;
    for (const f32 cz : {-1.0F, 1.0F})
    {
        for (const f32 cx : {-1.0F, 1.0F})
        {
            corner[index++] = builder.Add(
                {half[0] * cx, 0.0F, half[2] * cz},
                {0.0F, 1.0F, 0.0F},
                {1.0F, 0.0F, 0.0F},
                {0.5F + 0.5F * cx, 0.5F + 0.5F * cz});
        }
    }
    builder.Triangle(corner[0], corner[1], corner[2]);
    builder.Triangle(corner[1], corner[3], corner[2]);
}

void BuildSphere(Builder& builder, const Vec3& half)
{
    std::vector<ProfilePoint> profile;
    for (u32 i = 0U; i <= kSphereRings; ++i)
    {
        const f32 phi = std::numbers::pi_v<f32> * static_cast<f32>(i) /
                        static_cast<f32>(kSphereRings);
        const f32 r = std::sin(phi);
        const f32 y = std::cos(phi);
        profile.push_back({r, y, r, y});
    }
    Revolve(builder, profile, half[0], half[1], half[2]);
}

void BuildCylinder(Builder& builder, const Vec3& half)
{
    const std::vector<ProfilePoint> profile{
        {0.0F, 1.0F, 0.0F, 1.0F},
        {1.0F, 1.0F, 0.0F, 1.0F},
        {1.0F, 1.0F, 1.0F, 0.0F},
        {1.0F, -1.0F, 1.0F, 0.0F},
        {1.0F, -1.0F, 0.0F, -1.0F},
        {0.0F, -1.0F, 0.0F, -1.0F}};
    Revolve(builder, profile, half[0], half[1], half[2]);
}

void BuildCapsule(Builder& builder, const Vec3& half)
{
    const f32 radius = std::min(half[0], half[2]);
    const f32 shaft = std::max(half[1] - radius, 0.0F);

    std::vector<ProfilePoint> profile;
    for (u32 i = 0U; i <= kCapRings; ++i)
    {
        const f32 phi = 0.5F * std::numbers::pi_v<f32> *
                        static_cast<f32>(i) / static_cast<f32>(kCapRings);
        profile.push_back({
            radius * std::sin(phi),
            shaft + radius * std::cos(phi),
            std::sin(phi),
            std::cos(phi)});
    }
    for (u32 i = 0U; i <= kCapRings; ++i)
    {
        const f32 phi = 0.5F * std::numbers::pi_v<f32> +
                        0.5F * std::numbers::pi_v<f32> *
                            static_cast<f32>(i) /
                            static_cast<f32>(kCapRings);
        profile.push_back({
            radius * std::sin(phi),
            -shaft + radius * std::cos(phi),
            std::sin(phi),
            std::cos(phi)});
    }
    Revolve(builder, profile, 1.0F, 1.0F, 1.0F);
}

[[nodiscard]] bool PositiveFinite(const std::array<f64, 3>& size) noexcept
{
    return std::ranges::all_of(
        size,
        [](const f64 value) { return std::isfinite(value) && value > 0.0; });
}
} // namespace

MeshAsset BuildPrimitiveMesh(
    const PrimitiveMeshShape shape,
    const std::array<f64, 3>& sizeMeters,
    const MeshMaterial& material)
{
    Builder builder;
    builder.asset.name = "primitive";

    if (!PositiveFinite(sizeMeters))
    {
        return std::move(builder.asset);
    }

    const Vec3 half{
        static_cast<f32>(sizeMeters[0] * 0.5),
        static_cast<f32>(sizeMeters[1] * 0.5),
        static_cast<f32>(sizeMeters[2] * 0.5)};

    switch (shape)
    {
    case PrimitiveMeshShape::Box:
        BuildBox(builder, half);
        break;
    case PrimitiveMeshShape::Sphere:
        BuildSphere(builder, half);
        break;
    case PrimitiveMeshShape::Cylinder:
        BuildCylinder(builder, half);
        break;
    case PrimitiveMeshShape::Capsule:
        BuildCapsule(builder, half);
        break;
    case PrimitiveMeshShape::Plane:
        BuildPlane(builder, half);
        break;
    }

    MeshAsset asset = std::move(builder.asset);
    if (asset.indices.empty())
    {
        return asset;
    }

    asset.materials.push_back(material);
    asset.parts.push_back(
        {0U, static_cast<u32>(asset.indices.size()), 0U});

    asset.boundsMin = {1.0e30, 1.0e30, 1.0e30};
    asset.boundsMax = {-1.0e30, -1.0e30, -1.0e30};
    for (const MeshVertex& vertex : asset.vertices)
    {
        for (std::size_t axis = 0U; axis < 3U; ++axis)
        {
            asset.boundsMin[axis] = std::min(
                asset.boundsMin[axis],
                static_cast<f64>(vertex.position[axis]));
            asset.boundsMax[axis] = std::max(
                asset.boundsMax[axis],
                static_cast<f64>(vertex.position[axis]));
        }
    }
    return asset;
}
} // namespace orbit::mesh_import
