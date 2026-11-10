#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <orbit/mesh_render/ReflectionBvh.hpp>

namespace orbit::mesh_render
{
namespace
{
using V = math::Float3;
using T = lighting::ReflectionTriangle;
V xyz(const math::Float4 &v)
{
    return {v.x, v.y, v.z};
}
V add(V a, V b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
V sub(V a, V b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
V mul(V a, f32 b)
{
    return {a.x * b, a.y * b, a.z * b};
}
f32 dot(V a, V b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
V cross(V a, V b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
V unit(V a, V fallback)
{
    const auto l = std::sqrt(dot(a, a));
    return l > 1e-12F ? mul(a, 1.0F / l) : fallback;
}
math::Float4 vec(V v, f32 w = 0.0F)
{
    return {v.x, v.y, v.z, w};
}
f32 axis(V a, u32 i)
{
    return i == 0U ? a.x : i == 1U ? a.y : a.z;
}
V centroid(const T &t)
{
    return add(xyz(t.p0), mul(add(xyz(t.edge1), xyz(t.edge2)), 1.0F / 3.0F));
}
bool finite(V p)
{
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
bool box(const lighting::ReflectionBvhNode &n, V o, V d, f32 tmin, f32 tmax)
{
    for (u32 i = 0; i < 3; ++i)
    {
        const f32 a = axis(o, i), b = axis(d, i), lo = axis(n.minimum, i), hi = axis(n.maximum, i);
        if (std::abs(b) < 1e-12F)
        {
            if (a < lo || a > hi)
                return false;
        }
        else
        {
            const f32 x = (lo - a) / b, y = (hi - a) / b;
            tmin = std::max(tmin, std::min(x, y));
            tmax = std::min(tmax, std::max(x, y));
            if (tmin > tmax)
                return false;
        }
    }
    return true;
}
} // namespace
std::vector<T> BuildReflectionTriangles(const mesh_import::MeshAsset &asset)
{
    std::vector<T> result;
    for (const auto &part : asset.parts)
    {
        if (part.material >= asset.materials.size())
            continue;
        const auto &m = asset.materials[part.material];
        if (m.alphaMode != mesh_import::AlphaMode::Opaque)
            continue;
        const auto end = std::min<std::size_t>(
            asset.indices.size(), static_cast<std::size_t>(part.firstIndex) + part.indexCount);
        for (std::size_t i = part.firstIndex; i + 2 < end; i += 3)
        {
            const auto a = asset.indices[i], b = asset.indices[i + 1], c = asset.indices[i + 2];
            if (a >= asset.vertices.size() || b >= asset.vertices.size() ||
                c >= asset.vertices.size())
                continue;
            const auto v = [](const std::array<f32, 3> &p) -> V { return {p[0], p[1], p[2]}; };
            const auto &va = asset.vertices[a];
            const auto &vb = asset.vertices[b];
            const auto &vc = asset.vertices[c];
            const V p = v(va.position), e1 = sub(v(vb.position), p), e2 = sub(v(vc.position), p);
            if (!finite(p) || !finite(e1) || !finite(e2) ||
                dot(cross(e1, e2), cross(e1, e2)) < 1e-16F)
                continue;
            const V gn = unit(cross(e1, e2), {0, 1, 0});
            result.push_back({vec(p),
                              vec(e1),
                              vec(e2),
                              vec(unit(v(va.normal), gn)),
                              vec(unit(v(vb.normal), gn)),
                              vec(unit(v(vc.normal), gn)),
                              {m.baseColorFactor[0], m.baseColorFactor[1], m.baseColorFactor[2],
                               m.metallicFactor},
                              {m.emissiveFactor[0], m.emissiveFactor[1], m.emissiveFactor[2],
                               m.roughnessFactor}});
        }
    }
    return result;
}
T TransformReflectionTriangle(const T &t, const std::array<f32, 12> &r)
{
    const auto transform = [&](V p, f32 w) -> V {
        return {r[0] * p.x + r[1] * p.y + r[2] * p.z + r[3] * w,
                r[4] * p.x + r[5] * p.y + r[6] * p.z + r[7] * w,
                r[8] * p.x + r[9] * p.y + r[10] * p.z + r[11] * w};
    };
    T out = t;
    out.p0 = vec(transform(xyz(t.p0), 1));
    out.edge1 = vec(transform(xyz(t.edge1), 0));
    out.edge2 = vec(transform(xyz(t.edge2), 0));
    // MeshInstance accepts rotation + uniform scale, so this is also the
    // inverse-transpose normal direction.
    out.normal0 = vec(unit(transform(xyz(t.normal0), 0), {0, 1, 0}));
    out.normal1 = vec(unit(transform(xyz(t.normal1), 0), {0, 1, 0}));
    out.normal2 = vec(unit(transform(xyz(t.normal2), 0), {0, 1, 0}));
    return out;
}
ReflectionBvh BuildReflectionBvh(std::vector<T> triangles)
{
    ReflectionBvh bvh;
    bvh.triangles = std::move(triangles);
    if (bvh.triangles.empty())
        return bvh;
    std::function<void(u32, u32)> build = [&](u32 first, u32 count) {
        const u32 index = static_cast<u32>(bvh.nodes.size());
        bvh.nodes.emplace_back();
        V lo{1e30F, 1e30F, 1e30F}, hi{-1e30F, -1e30F, -1e30F};
        for (u32 i = first; i < first + count; ++i)
        {
            const T &t = bvh.triangles[i];
            for (V p : {xyz(t.p0), add(xyz(t.p0), xyz(t.edge1)), add(xyz(t.p0), xyz(t.edge2))})
            {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        }
        bvh.nodes[index].minimum = lo;
        bvh.nodes[index].maximum = hi;
        if (count <= 4U)
        {
            bvh.nodes[index].first = first;
            bvh.nodes[index].count = count;
        }
        else
        {
            const V extent = sub(hi, lo);
            const u32 a = extent.x >= extent.y && extent.x >= extent.z ? 0U
                          : extent.y >= extent.z                       ? 1U
                                                                       : 2U;
            const u32 half = count / 2U;
            std::nth_element(bvh.triangles.begin() + first, bvh.triangles.begin() + first + half,
                             bvh.triangles.begin() + first + count, [a](const T &x, const T &y) {
                                 return axis(centroid(x), a) < axis(centroid(y), a);
                             });
            build(first, half);
            build(first + half, count - half);
        }
        bvh.nodes[index].escape = static_cast<u32>(bvh.nodes.size());
    };
    build(0U, static_cast<u32>(bvh.triangles.size()));
    return bvh;
}
ReflectionHit TraceReflectionBvh(const ReflectionBvh &bvh, V o, V d, f32 tmin, f32 tmax)
{
    ReflectionHit result;
    result.distance = tmax;
    if (!finite(o) || !finite(d) || dot(d, d) < 1e-16F || tmin < 0 || tmin > tmax)
        return result;
    d = unit(d, {0, 0, 1});
    for (u32 node = 0; node < bvh.nodes.size();)
    {
        const auto &n = bvh.nodes[node];
        if (!box(n, o, d, tmin, result.distance))
        {
            node = n.escape;
            continue;
        }
        for (u32 i = n.first; i < n.first + n.count; ++i)
        {
            const T &t = bvh.triangles[i];
            const V e1 = xyz(t.edge1), e2 = xyz(t.edge2), p = cross(d, e2);
            const f32 det = dot(e1, p);
            if (std::abs(det) < 1e-10F)
                continue;
            const f32 inv = 1 / det;
            const V s = sub(o, xyz(t.p0));
            const f32 u = dot(s, p) * inv;
            const V q = cross(s, e1);
            const f32 v = dot(d, q) * inv, h = dot(e2, q) * inv;
            if (u < 0 || v < 0 || u + v > 1 || h < tmin || h > result.distance)
                continue;
            result.hit = true;
            result.distance = h;
            result.triangle = i;
            result.normal = unit(add(add(mul(xyz(t.normal0), 1 - u - v), mul(xyz(t.normal1), u)),
                                     mul(xyz(t.normal2), v)),
                                 unit(cross(e1, e2), {0, 1, 0}));
            if (dot(result.normal, d) > 0)
                result.normal = mul(result.normal, -1);
        }
        ++node;
    }
    return result;
}
} // namespace orbit::mesh_render
