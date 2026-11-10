#include <orbit/mesh_sdf/MeshSdf.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

namespace orbit::mesh_sdf
{
namespace
{
struct Vec3
{
    f32 x{0.0F};
    f32 y{0.0F};
    f32 z{0.0F};
};

[[nodiscard]] Vec3 operator+(const Vec3& a, const Vec3& b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] Vec3 operator-(const Vec3& a, const Vec3& b) noexcept
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] Vec3 operator*(const Vec3& a, const f32 s) noexcept
{
    return {a.x * s, a.y * s, a.z * s};
}
[[nodiscard]] f32 Dot(const Vec3& a, const Vec3& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] Vec3 Cross(const Vec3& a, const Vec3& b) noexcept
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}
[[nodiscard]] Vec3 Normalized(const Vec3& v, const Vec3& fallback) noexcept
{
    const f32 length = std::sqrt(Dot(v, v));
    return length > 1.0e-20F ? v * (1.0F / length) : fallback;
}

[[nodiscard]] Vec3 FromArray(const std::array<f32, 3>& a) noexcept
{
    return {a[0], a[1], a[2]};
}

struct Triangle
{
    Vec3 a;
    Vec3 b;
    Vec3 c;
    std::array<u32, 3> vertex{};
    u32 material{0U};
};

struct Node
{
    Vec3 minimum;
    Vec3 maximum;
    // Interior: first = left child index, count = 0 (right = left + 1).
    // Leaf: first = first triangle in `order`, count > 0.
    u32 first{0U};
    u32 count{0U};
};

// Squared distance from a point to an axis-aligned box.
[[nodiscard]] f32 BoxDistanceSquared(
    const Vec3& p,
    const Vec3& lo,
    const Vec3& hi) noexcept
{
    const f32 dx = std::max({lo.x - p.x, 0.0F, p.x - hi.x});
    const f32 dy = std::max({lo.y - p.y, 0.0F, p.y - hi.y});
    const f32 dz = std::max({lo.z - p.z, 0.0F, p.z - hi.z});
    return dx * dx + dy * dy + dz * dz;
}

struct Closest
{
    f32 distanceSquared{std::numeric_limits<f32>::max()};
    Vec3 point;
    u32 triangle{0xFFFFFFFFU};
    // Barycentric weights of a, b, c at `point`.
    f32 wa{1.0F};
    f32 wb{0.0F};
    f32 wc{0.0F};
};

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection).
void ClosestOnTriangle(
    const Vec3& p,
    const Triangle& t,
    Vec3& outPoint,
    f32& wa,
    f32& wb,
    f32& wc) noexcept
{
    const Vec3 ab = t.b - t.a;
    const Vec3 ac = t.c - t.a;
    const Vec3 ap = p - t.a;
    const f32 d1 = Dot(ab, ap);
    const f32 d2 = Dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F)
    {
        outPoint = t.a;
        wa = 1.0F;
        wb = 0.0F;
        wc = 0.0F;
        return;
    }

    const Vec3 bp = p - t.b;
    const f32 d3 = Dot(ab, bp);
    const f32 d4 = Dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3)
    {
        outPoint = t.b;
        wa = 0.0F;
        wb = 1.0F;
        wc = 0.0F;
        return;
    }

    const f32 vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F)
    {
        const f32 v = d1 / (d1 - d3);
        outPoint = t.a + ab * v;
        wa = 1.0F - v;
        wb = v;
        wc = 0.0F;
        return;
    }

    const Vec3 cp = p - t.c;
    const f32 d5 = Dot(ab, cp);
    const f32 d6 = Dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6)
    {
        outPoint = t.c;
        wa = 0.0F;
        wb = 0.0F;
        wc = 1.0F;
        return;
    }

    const f32 vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F)
    {
        const f32 w = d2 / (d2 - d6);
        outPoint = t.a + ac * w;
        wa = 1.0F - w;
        wb = 0.0F;
        wc = w;
        return;
    }

    const f32 va = d3 * d6 - d5 * d4;
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F)
    {
        const f32 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        outPoint = t.b + (t.c - t.b) * w;
        wa = 0.0F;
        wb = 1.0F - w;
        wc = w;
        return;
    }

    const f32 denominator = 1.0F / (va + vb + vc);
    const f32 v = vb * denominator;
    const f32 w = vc * denominator;
    outPoint = t.a + ab * v + ac * w;
    wa = 1.0F - v - w;
    wb = v;
    wc = w;
}

class Bvh
{
public:
    explicit Bvh(const std::vector<Triangle>& triangles)
        : triangles_(triangles)
    {
        order_.resize(triangles.size());
        for (u32 i = 0U; i < order_.size(); ++i)
        {
            order_[i] = i;
        }
        if (!triangles.empty())
        {
            nodes_.reserve(triangles.size());
            Build(0U, static_cast<u32>(triangles.size()));
        }
    }

    [[nodiscard]] Closest Nearest(const Vec3& p) const noexcept
    {
        Closest best;
        if (nodes_.empty())
        {
            return best;
        }

        std::array<u32, 64> stack{};
        u32 top = 0U;
        stack[top++] = 0U;

        while (top > 0U)
        {
            const Node& node = nodes_[stack[--top]];
            if (BoxDistanceSquared(p, node.minimum, node.maximum) >=
                best.distanceSquared)
            {
                continue;
            }

            if (node.count > 0U)
            {
                for (u32 i = 0U; i < node.count; ++i)
                {
                    const u32 index = order_[node.first + i];
                    Vec3 point;
                    f32 wa = 0.0F;
                    f32 wb = 0.0F;
                    f32 wc = 0.0F;
                    ClosestOnTriangle(p, triangles_[index], point, wa, wb, wc);
                    const Vec3 delta = p - point;
                    const f32 d2 = Dot(delta, delta);
                    if (d2 < best.distanceSquared)
                    {
                        best.distanceSquared = d2;
                        best.point = point;
                        best.triangle = index;
                        best.wa = wa;
                        best.wb = wb;
                        best.wc = wc;
                    }
                }
                continue;
            }

            const Node& left = nodes_[node.first];
            const Node& right = nodes_[node.first + 1U];
            const f32 dl = BoxDistanceSquared(p, left.minimum, left.maximum);
            const f32 dr = BoxDistanceSquared(p, right.minimum, right.maximum);
            // Push the farther child first so the nearer pops first.
            if (dl < dr)
            {
                stack[top++] = node.first + 1U;
                stack[top++] = node.first;
            }
            else
            {
                stack[top++] = node.first;
                stack[top++] = node.first + 1U;
            }
        }
        return best;
    }

private:
    u32 Build(const u32 begin, const u32 end)
    {
        const auto nodeIndex = static_cast<u32>(nodes_.size());
        nodes_.emplace_back();

        Vec3 lo{1.0e30F, 1.0e30F, 1.0e30F};
        Vec3 hi{-1.0e30F, -1.0e30F, -1.0e30F};
        Vec3 clo = lo;
        Vec3 chi = hi;
        for (u32 i = begin; i < end; ++i)
        {
            const Triangle& t = triangles_[order_[i]];
            for (const Vec3* v : {&t.a, &t.b, &t.c})
            {
                lo = {std::min(lo.x, v->x), std::min(lo.y, v->y),
                      std::min(lo.z, v->z)};
                hi = {std::max(hi.x, v->x), std::max(hi.y, v->y),
                      std::max(hi.z, v->z)};
            }
            const Vec3 c = (t.a + t.b + t.c) * (1.0F / 3.0F);
            clo = {std::min(clo.x, c.x), std::min(clo.y, c.y),
                   std::min(clo.z, c.z)};
            chi = {std::max(chi.x, c.x), std::max(chi.y, c.y),
                   std::max(chi.z, c.z)};
        }
        nodes_[nodeIndex].minimum = lo;
        nodes_[nodeIndex].maximum = hi;

        constexpr u32 kLeafSize = 4U;
        if (end - begin <= kLeafSize)
        {
            nodes_[nodeIndex].first = begin;
            nodes_[nodeIndex].count = end - begin;
            return nodeIndex;
        }

        const Vec3 extent = chi - clo;
        u32 axis = 0U;
        if (extent.y > extent.x && extent.y >= extent.z)
        {
            axis = 1U;
        }
        else if (extent.z > extent.x && extent.z > extent.y)
        {
            axis = 2U;
        }

        const auto centroid = [&](const u32 index)
        {
            const Triangle& t = triangles_[index];
            const Vec3 c = (t.a + t.b + t.c) * (1.0F / 3.0F);
            return axis == 0U ? c.x : axis == 1U ? c.y : c.z;
        };

        const u32 middle = begin + (end - begin) / 2U;
        std::nth_element(
            order_.begin() + begin,
            order_.begin() + middle,
            order_.begin() + end,
            [&](const u32 l, const u32 r) { return centroid(l) < centroid(r); });

        const u32 left = Build(begin, middle);
        const u32 right = Build(middle, end);
        // Children are built consecutively only if the left subtree is a
        // single node; store both indices instead via a remap node pair.
        nodes_[nodeIndex].first = left;
        nodes_[nodeIndex].count = 0U;
        rightOf_.push_back({nodeIndex, right});
        return nodeIndex;
    }

public:
    // Re-lays the tree so the two children of every interior node are
    // adjacent (child pair at first, first + 1), which Nearest() relies on.
    void Finalize()
    {
        if (nodes_.empty())
        {
            return;
        }
        std::vector<u32> rightIndex(nodes_.size(), 0U);
        for (const auto& [parent, right] : rightOf_)
        {
            rightIndex[parent] = right;
        }

        std::vector<Node> out;
        out.reserve(nodes_.size());
        out.push_back(nodes_[0]);

        // (source node, destination index)
        std::vector<std::pair<u32, u32>> queue{{0U, 0U}};
        for (std::size_t head = 0U; head < queue.size(); ++head)
        {
            const auto [source, destination] = queue[head];
            const Node& node = nodes_[source];
            if (node.count > 0U)
            {
                continue;
            }
            const auto first = static_cast<u32>(out.size());
            out.push_back(nodes_[node.first]);
            out.push_back(nodes_[rightIndex[source]]);
            out[destination].first = first;
            queue.push_back({node.first, first});
            queue.push_back({rightIndex[source], first + 1U});
        }
        nodes_ = std::move(out);
        rightOf_.clear();
    }

private:
    const std::vector<Triangle>& triangles_;
    std::vector<u32> order_;
    std::vector<Node> nodes_;
    std::vector<std::pair<u32, u32>> rightOf_;
};

// ---- textures --------------------------------------------------------------

[[nodiscard]] f32 SrgbToLinear(const f32 v) noexcept
{
    return v <= 0.04045F ? v / 12.92F
                         : std::pow((v + 0.055F) / 1.055F, 2.4F);
}

struct MipImage
{
    // Linear RGBA floats per level, level 0 = full size.
    struct Level
    {
        u32 width{1U};
        u32 height{1U};
        std::vector<f32> rgba;
    };
    std::vector<Level> levels;
    bool valid{false};
};

[[nodiscard]] MipImage BuildMips(const SdfImage& image, const bool srgb)
{
    MipImage mip;
    if (image.width == 0U || image.height == 0U ||
        image.rgba.size() <
            static_cast<std::size_t>(image.width) * image.height * 4U)
    {
        return mip;
    }

    MipImage::Level base;
    base.width = image.width;
    base.height = image.height;
    base.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4U);
    for (std::size_t i = 0U; i < base.rgba.size(); ++i)
    {
        const f32 value =
            static_cast<f32>(static_cast<u8>(image.rgba[i])) / 255.0F;
        base.rgba[i] = (srgb && (i % 4U) != 3U) ? SrgbToLinear(value) : value;
    }
    mip.levels.push_back(std::move(base));

    while (mip.levels.back().width > 1U || mip.levels.back().height > 1U)
    {
        const auto& src = mip.levels.back();
        MipImage::Level next;
        next.width = std::max(src.width / 2U, 1U);
        next.height = std::max(src.height / 2U, 1U);
        next.rgba.assign(
            static_cast<std::size_t>(next.width) * next.height * 4U, 0.0F);
        for (u32 y = 0U; y < next.height; ++y)
        {
            for (u32 x = 0U; x < next.width; ++x)
            {
                for (u32 channel = 0U; channel < 4U; ++channel)
                {
                    f32 sum = 0.0F;
                    for (u32 dy = 0U; dy < 2U; ++dy)
                    {
                        for (u32 dx = 0U; dx < 2U; ++dx)
                        {
                            const u32 sx = std::min(x * 2U + dx, src.width - 1U);
                            const u32 sy = std::min(y * 2U + dy, src.height - 1U);
                            sum += src.rgba[(static_cast<std::size_t>(sy) *
                                                 src.width + sx) * 4U + channel];
                        }
                    }
                    next.rgba[(static_cast<std::size_t>(y) * next.width + x) *
                                  4U + channel] = sum * 0.25F;
                }
            }
        }
        mip.levels.push_back(std::move(next));
    }
    mip.valid = true;
    return mip;
}

[[nodiscard]] std::array<f32, 4> SampleMip(
    const MipImage& image,
    const f32 u,
    const f32 v,
    const f32 level) noexcept
{
    if (!image.valid)
    {
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }

    const auto clamped = static_cast<std::size_t>(std::clamp(
        std::round(level), 0.0F, static_cast<f32>(image.levels.size() - 1U)));
    const auto& l = image.levels[clamped];

    const f32 fu = u - std::floor(u);
    const f32 fv = v - std::floor(v);
    const u32 x = std::min(static_cast<u32>(fu * static_cast<f32>(l.width)),
                           l.width - 1U);
    const u32 y = std::min(static_cast<u32>(fv * static_cast<f32>(l.height)),
                           l.height - 1U);
    const std::size_t at = (static_cast<std::size_t>(y) * l.width + x) * 4U;
    return {l.rgba[at], l.rgba[at + 1U], l.rgba[at + 2U], l.rgba[at + 3U]};
}

[[nodiscard]] u32 PackColor(const f32 r, const f32 g, const f32 b, const f32 a)
{
    const auto q = [](const f32 v)
    {
        return static_cast<u32>(
            std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
    };
    return q(r) | (q(g) << 8U) | (q(b) << 16U) | (q(a) << 24U);
}
} // namespace

u32 PackOctahedralNormal(const f32 x, const f32 y, const f32 z) noexcept
{
    const f32 inverse = 1.0F / std::max(std::abs(x) + std::abs(y) + std::abs(z),
                                        1.0e-20F);
    f32 px = x * inverse;
    f32 py = y * inverse;
    if (z < 0.0F)
    {
        const f32 ox = (1.0F - std::abs(py)) * (px >= 0.0F ? 1.0F : -1.0F);
        const f32 oy = (1.0F - std::abs(px)) * (py >= 0.0F ? 1.0F : -1.0F);
        px = ox;
        py = oy;
    }
    const auto q = [](const f32 v)
    {
        return static_cast<u32>(static_cast<i32>(std::lround(
                   std::clamp(v, -1.0F, 1.0F) * 32767.0F)) & 0xFFFF);
    };
    return q(px) | (q(py) << 16U);
}

std::array<f32, 3> UnpackOctahedralNormal(const u32 packed) noexcept
{
    const auto dq = [](const u32 bits)
    {
        return static_cast<f32>(static_cast<i16>(bits & 0xFFFFU)) / 32767.0F;
    };
    f32 x = dq(packed);
    f32 y = dq(packed >> 16U);
    f32 z = 1.0F - std::abs(x) - std::abs(y);
    if (z < 0.0F)
    {
        const f32 ox = (1.0F - std::abs(y)) * (x >= 0.0F ? 1.0F : -1.0F);
        const f32 oy = (1.0F - std::abs(x)) * (y >= 0.0F ? 1.0F : -1.0F);
        x = ox;
        y = oy;
    }
    const f32 length = std::sqrt(x * x + y * y + z * z);
    const f32 inverse = length > 1.0e-20F ? 1.0F / length : 1.0F;
    return {x * inverse, y * inverse, z * inverse};
}

MeshSdf BuildMeshSdf(
    const mesh_import::MeshAsset& asset,
    const std::span<const SdfImage> images,
    const SdfSettings& settings)
{
    MeshSdf sdf;

    // Triangles with their material.
    std::vector<Triangle> triangles;
    triangles.reserve(asset.indices.size() / 3U);
    for (const auto& part : asset.parts)
    {
        for (u32 i = 0U; i + 2U < part.indexCount; i += 3U)
        {
            const u32 i0 = asset.indices[part.firstIndex + i];
            const u32 i1 = asset.indices[part.firstIndex + i + 1U];
            const u32 i2 = asset.indices[part.firstIndex + i + 2U];
            Triangle t;
            t.a = FromArray(asset.vertices[i0].position);
            t.b = FromArray(asset.vertices[i1].position);
            t.c = FromArray(asset.vertices[i2].position);
            const Vec3 normal = Cross(t.b - t.a, t.c - t.a);
            if (Dot(normal, normal) <= 1.0e-18F)
            {
                continue; // degenerate
            }
            t.vertex = {i0, i1, i2};
            t.material = part.material;
            triangles.push_back(t);
        }
    }
    if (triangles.empty())
    {
        return sdf;
    }

    // Grid: asset bounds + margin, coarsened to the limits.
    const Vec3 lo = FromArray({
        static_cast<f32>(asset.boundsMin[0]),
        static_cast<f32>(asset.boundsMin[1]),
        static_cast<f32>(asset.boundsMin[2])});
    const Vec3 hi = FromArray({
        static_cast<f32>(asset.boundsMax[0]),
        static_cast<f32>(asset.boundsMax[1]),
        static_cast<f32>(asset.boundsMax[2])});

    f32 voxel = std::max(settings.targetVoxelMeters, 1.0e-3F);
    const f32 margin = static_cast<f32>(settings.marginVoxels);
    std::array<u32, 3> dims{};
    for (int attempt = 0; attempt < 32; ++attempt)
    {
        const f32 extents[3] = {hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
        u64 total = 1U;
        bool tooBig = false;
        for (int axis = 0; axis < 3; ++axis)
        {
            dims[static_cast<std::size_t>(axis)] = static_cast<u32>(
                std::ceil(extents[axis] / voxel) + 2.0F * margin + 1.0F);
            total *= dims[static_cast<std::size_t>(axis)];
            tooBig = tooBig || dims[static_cast<std::size_t>(axis)] >
                                   settings.maximumDimension;
        }
        if (!tooBig && total <= settings.maximumVoxels)
        {
            break;
        }
        voxel *= 1.15F;
    }

    sdf.dimensions = dims;
    sdf.voxelSize = voxel;
    sdf.origin = {
        lo.x - margin * voxel,
        lo.y - margin * voxel,
        lo.z - margin * voxel};
    sdf.surfaceBand = voxel * 0.87F;

    const std::size_t count = sdf.VoxelCount();
    sdf.distance.assign(count, 0.0F);
    sdf.albedo.assign(count, 0U);
    sdf.normal.assign(count, 0U);
    sdf.emissive.assign(count, 0U);

    // Material textures as mip chains (base colour and emissive are sRGB).
    std::vector<MipImage> baseMips(images.size());
    std::vector<MipImage> emissiveMips(images.size());
    const auto baseMipFor = [&](const i32 textureIndex) -> const MipImage*
    {
        if (textureIndex < 0 ||
            static_cast<std::size_t>(textureIndex) >= asset.textures.size())
        {
            return nullptr;
        }
        const i32 image = asset.textures[static_cast<std::size_t>(textureIndex)].image;
        if (image < 0 || static_cast<std::size_t>(image) >= images.size())
        {
            return nullptr;
        }
        auto& slot = baseMips[static_cast<std::size_t>(image)];
        if (!slot.valid)
        {
            slot = BuildMips(images[static_cast<std::size_t>(image)], true);
        }
        return slot.valid ? &slot : nullptr;
    };
    const auto emissiveMipFor = [&](const i32 textureIndex) -> const MipImage*
    {
        if (textureIndex < 0 ||
            static_cast<std::size_t>(textureIndex) >= asset.textures.size())
        {
            return nullptr;
        }
        const i32 image = asset.textures[static_cast<std::size_t>(textureIndex)].image;
        if (image < 0 || static_cast<std::size_t>(image) >= images.size())
        {
            return nullptr;
        }
        auto& slot = emissiveMips[static_cast<std::size_t>(image)];
        if (!slot.valid)
        {
            slot = BuildMips(images[static_cast<std::size_t>(image)], true);
        }
        return slot.valid ? &slot : nullptr;
    };

    // Build every needed mip chain up front (single-threaded), so the
    // parallel loop only reads.
    std::vector<const MipImage*> materialBase(asset.materials.size(), nullptr);
    std::vector<const MipImage*> materialEmissive(asset.materials.size(), nullptr);
    f32 emissiveMax = 0.0F;
    for (std::size_t m = 0U; m < asset.materials.size(); ++m)
    {
        materialBase[m] = baseMipFor(asset.materials[m].baseColorTexture.texture);
        materialEmissive[m] =
            emissiveMipFor(asset.materials[m].emissiveTexture.texture);
        if (!asset.materials[m].emissiveInGi)
        {
            continue;
        }
        for (const f32 c : asset.materials[m].emissiveFactor)
        {
            emissiveMax = std::max(emissiveMax, c);
        }
    }
    sdf.emissiveScale = std::max(emissiveMax, 1.0F);

    Bvh bvh(triangles);
    bvh.Finalize();

    const u32 threadCount = settings.threads != 0U
        ? settings.threads
        : std::max(1U, std::thread::hardware_concurrency());
    std::atomic<u32> nextSlice{0U};
    std::atomic<u32> surfaceVoxels{0U};
    std::vector<f32> maxima(threadCount, 0.0F);

    const auto worker = [&](const u32 workerIndex)
    {
        f32 localMax = 0.0F;
        u32 localSurface = 0U;
        for (;;)
        {
            const u32 z = nextSlice.fetch_add(1U);
            if (z >= dims[2])
            {
                break;
            }
            for (u32 y = 0U; y < dims[1]; ++y)
            {
                for (u32 x = 0U; x < dims[0]; ++x)
                {
                    const Vec3 p{
                        sdf.origin[0] + static_cast<f32>(x) * voxel,
                        sdf.origin[1] + static_cast<f32>(y) * voxel,
                        sdf.origin[2] + static_cast<f32>(z) * voxel};
                    const Closest hit = bvh.Nearest(p);
                    const std::size_t at = sdf.Index(x, y, z);
                    const f32 distance = std::sqrt(hit.distanceSquared);
                    sdf.distance[at] = distance;
                    localMax = std::max(localMax, distance);

                    if (distance > sdf.surfaceBand ||
                        hit.triangle == 0xFFFFFFFFU)
                    {
                        continue;
                    }

                    ++localSurface;
                    const Triangle& t = triangles[hit.triangle];
                    const auto& va = asset.vertices[t.vertex[0]];
                    const auto& vb = asset.vertices[t.vertex[1]];
                    const auto& vc = asset.vertices[t.vertex[2]];

                    // Normal: interpolated vertex normal, flipped toward p.
                    Vec3 n{
                        va.normal[0] * hit.wa + vb.normal[0] * hit.wb +
                            vc.normal[0] * hit.wc,
                        va.normal[1] * hit.wa + vb.normal[1] * hit.wb +
                            vc.normal[1] * hit.wc,
                        va.normal[2] * hit.wa + vb.normal[2] * hit.wb +
                            vc.normal[2] * hit.wc};
                    const Vec3 geometric = Normalized(
                        Cross(t.b - t.a, t.c - t.a), {0.0F, 1.0F, 0.0F});
                    n = Normalized(n, geometric);
                    if (distance > 1.0e-4F)
                    {
                        if (Dot(n, p - hit.point) < 0.0F)
                        {
                            n = n * -1.0F;
                        }
                    }
                    sdf.normal[at] = PackOctahedralNormal(n.x, n.y, n.z);

                    // Albedo: material factor times the texture colour at a
                    // mip matching the voxel footprint.
                    const auto& material = asset.materials[std::min<std::size_t>(
                        t.material, asset.materials.size() - 1U)];
                    const f32 u = va.uv[0] * hit.wa + vb.uv[0] * hit.wb +
                                  vc.uv[0] * hit.wc;
                    const f32 v = va.uv[1] * hit.wa + vb.uv[1] * hit.wb +
                                  vc.uv[1] * hit.wc;

                    f32 mipLevel = 0.0F;
                    {
                        const f32 du1 = vb.uv[0] - va.uv[0];
                        const f32 dv1 = vb.uv[1] - va.uv[1];
                        const f32 du2 = vc.uv[0] - va.uv[0];
                        const f32 dv2 = vc.uv[1] - va.uv[1];
                        const f32 uvArea = std::abs(du1 * dv2 - du2 * dv1);
                        const Vec3 cross = Cross(t.b - t.a, t.c - t.a);
                        const f32 worldArea = std::sqrt(Dot(cross, cross));
                        if (worldArea > 1.0e-12F && uvArea > 0.0F)
                        {
                            const f32 uvPerMeter = std::sqrt(uvArea / worldArea);
                            const f32 footprintUv = voxel * uvPerMeter;
                            const MipImage* base =
                                materialBase[std::min<std::size_t>(
                                    t.material, materialBase.size() - 1U)];
                            const f32 size = base != nullptr
                                ? static_cast<f32>(std::max(
                                      base->levels[0].width,
                                      base->levels[0].height))
                                : 1.0F;
                            mipLevel = std::max(
                                std::log2(std::max(footprintUv * size, 1.0F)),
                                0.0F);
                        }
                    }

                    std::array<f32, 4> texel{1.0F, 1.0F, 1.0F, 1.0F};
                    if (const MipImage* base = materialBase[std::min<std::size_t>(
                            t.material, materialBase.size() - 1U)];
                        base != nullptr)
                    {
                        texel = SampleMip(*base, u, v, mipLevel);
                    }
                    sdf.albedo[at] = PackColor(
                        material.baseColorFactor[0] * texel[0],
                        material.baseColorFactor[1] * texel[1],
                        material.baseColorFactor[2] * texel[2],
                        1.0F);

                    if (material.emissiveInGi &&
                        (material.emissiveFactor[0] > 0.0F ||
                         material.emissiveFactor[1] > 0.0F ||
                         material.emissiveFactor[2] > 0.0F))
                    {
                        std::array<f32, 4> glow{1.0F, 1.0F, 1.0F, 1.0F};
                        if (const MipImage* emissive =
                                materialEmissive[std::min<std::size_t>(
                                    t.material, materialEmissive.size() - 1U)];
                            emissive != nullptr)
                        {
                            glow = SampleMip(*emissive, u, v, mipLevel);
                        }
                        const f32 inverse = 1.0F / sdf.emissiveScale;
                        sdf.emissive[at] = PackColor(
                            material.emissiveFactor[0] * glow[0] * inverse,
                            material.emissiveFactor[1] * glow[1] * inverse,
                            material.emissiveFactor[2] * glow[2] * inverse,
                            0.0F);
                    }
                }
            }
        }
        maxima[workerIndex] = localMax;
        surfaceVoxels.fetch_add(localSurface);
    };

    std::vector<std::thread> threads;
    for (u32 i = 1U; i < threadCount; ++i)
    {
        threads.emplace_back(worker, i);
    }
    worker(0U);
    for (auto& thread : threads)
    {
        thread.join();
    }

    sdf.maximumDistance = *std::max_element(maxima.begin(), maxima.end());
    sdf.surfaceVoxelCount = surfaceVoxels.load();
    return sdf;
}
} // namespace orbit::mesh_sdf
