#include <orbit/shading/ObjMesh.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <unordered_map>

namespace orbit::shading
{
namespace
{
struct Float3
{
    f32 x{0.0F};
    f32 y{0.0F};
    f32 z{0.0F};
};

[[nodiscard]] std::runtime_error Error(
    const u32 line,
    const std::string& message)
{
    return std::runtime_error("line " + std::to_string(line) + ": " + message);
}

[[nodiscard]] std::vector<std::string_view> Split(const std::string_view line)
{
    std::vector<std::string_view> tokens;
    std::size_t index = 0U;

    while (index < line.size())
    {
        while (index < line.size() &&
               (line[index] == ' ' || line[index] == '\t' ||
                line[index] == '\r'))
        {
            ++index;
        }

        const std::size_t start = index;
        while (index < line.size() && line[index] != ' ' &&
               line[index] != '\t' && line[index] != '\r')
        {
            ++index;
        }

        if (index > start)
        {
            tokens.push_back(line.substr(start, index - start));
        }
    }

    return tokens;
}

[[nodiscard]] f32 Number(const std::string_view token, const u32 line)
{
    f32 value = 0.0F;
    const auto [end, code] =
        std::from_chars(token.data(), token.data() + token.size(), value);

    if (code != std::errc{} || end != token.data() + token.size() ||
        !std::isfinite(value))
    {
        throw Error(line, "'" + std::string(token) + "' is not a finite number.");
    }
    return value;
}

// Resolves a 1-based (or negative, relative) OBJ index to a 0-based one.
[[nodiscard]] i64 Resolve(
    const std::string_view token,
    const std::size_t count,
    const u32 line,
    const char* what)
{
    i64 value = 0;
    const auto [end, code] =
        std::from_chars(token.data(), token.data() + token.size(), value);

    if (code != std::errc{} || end != token.data() + token.size() || value == 0)
    {
        throw Error(line, std::string("bad ") + what + " index '" +
                              std::string(token) + "'.");
    }

    const i64 resolved =
        value > 0 ? value - 1 : static_cast<i64>(count) + value;

    if (resolved < 0 || resolved >= static_cast<i64>(count))
    {
        throw Error(line, std::string(what) + " index " + std::to_string(value) +
                              " is out of range (" + std::to_string(count) +
                              " defined).");
    }
    return resolved;
}

struct CornerKey
{
    i64 position{-1};
    i64 uv{-1};
    i64 normal{-1};

    [[nodiscard]] bool operator==(const CornerKey&) const noexcept = default;
};

struct CornerHash
{
    [[nodiscard]] std::size_t operator()(const CornerKey& key) const noexcept
    {
        std::size_t hash = 1469598103934665603ULL;
        for (const i64 part : {key.position, key.uv, key.normal})
        {
            hash ^= static_cast<std::size_t>(part) + 0x9e3779b97f4a7c15ULL +
                (hash << 6U) + (hash >> 2U);
        }
        return hash;
    }
};

[[nodiscard]] Float3 Cross(const Float3& a, const Float3& b) noexcept
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}

[[nodiscard]] Float3 Normalise(const Float3& v) noexcept
{
    const f32 length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length < 1.0e-20F)
    {
        return {0.0F, 1.0F, 0.0F};
    }
    return {v.x / length, v.y / length, v.z / length};
}
} // namespace

MeshData ParseObj(const std::string_view text)
{
    std::vector<Float3> positions;
    std::vector<Float3> normals;
    std::vector<std::array<f32, 2>> uvs;

    // Triangles as three corners each.
    std::vector<std::array<CornerKey, 3>> triangles;
    u32 faceCount = 0U;

    u32 lineNumber = 0U;
    std::size_t cursor = 0U;

    while (cursor < text.size())
    {
        const std::size_t end = text.find('\n', cursor);
        std::string_view line = text.substr(
            cursor,
            end == std::string_view::npos ? std::string_view::npos : end - cursor);
        cursor = end == std::string_view::npos ? text.size() : end + 1U;
        ++lineNumber;

        if (const auto hash = line.find('#'); hash != std::string_view::npos)
        {
            line = line.substr(0U, hash);
        }

        const auto tokens = Split(line);
        if (tokens.empty())
        {
            continue;
        }

        const std::string_view record = tokens[0];

        if (record == "v")
        {
            if (tokens.size() < 4U)
            {
                throw Error(lineNumber, "'v' needs x y z.");
            }
            positions.push_back({
                Number(tokens[1], lineNumber),
                Number(tokens[2], lineNumber),
                Number(tokens[3], lineNumber)});
        }
        else if (record == "vn")
        {
            if (tokens.size() < 4U)
            {
                throw Error(lineNumber, "'vn' needs x y z.");
            }
            normals.push_back(Normalise({
                Number(tokens[1], lineNumber),
                Number(tokens[2], lineNumber),
                Number(tokens[3], lineNumber)}));
        }
        else if (record == "vt")
        {
            if (tokens.size() < 3U)
            {
                throw Error(lineNumber, "'vt' needs u v.");
            }
            uvs.push_back({
                Number(tokens[1], lineNumber),
                Number(tokens[2], lineNumber)});
        }
        else if (record == "f")
        {
            if (tokens.size() < 4U)
            {
                throw Error(lineNumber, "'f' needs at least three vertices.");
            }

            std::vector<CornerKey> corners;
            for (std::size_t index = 1U; index < tokens.size(); ++index)
            {
                // v, v/vt, v//vn or v/vt/vn
                const std::string_view token = tokens[index];
                std::array<std::string_view, 3> part{};
                std::size_t count = 0U;
                std::size_t start = 0U;

                for (std::size_t at = 0U; at <= token.size(); ++at)
                {
                    if (at == token.size() || token[at] == '/')
                    {
                        if (count >= 3U)
                        {
                            throw Error(lineNumber, "too many '/' in '" +
                                                        std::string(token) + "'.");
                        }
                        part[count++] = token.substr(start, at - start);
                        start = at + 1U;
                    }
                }

                CornerKey key;
                key.position =
                    Resolve(part[0], positions.size(), lineNumber, "vertex");
                if (count >= 2U && !part[1].empty())
                {
                    key.uv = Resolve(part[1], uvs.size(), lineNumber, "texcoord");
                }
                if (count >= 3U && !part[2].empty())
                {
                    key.normal =
                        Resolve(part[2], normals.size(), lineNumber, "normal");
                }
                corners.push_back(key);
            }

            for (std::size_t index = 1U; index + 1U < corners.size(); ++index)
            {
                triangles.push_back({corners[0], corners[index], corners[index + 1U]});
            }
            ++faceCount;
        }
    }

    if (triangles.empty())
    {
        throw std::runtime_error("The file contains no faces.");
    }

    // Smooth normals for corners without a `vn`, shared per position.
    std::vector<Float3> smooth(positions.size());
    bool anyMissingNormal = false;

    for (const auto& triangle : triangles)
    {
        if (triangle[0].normal >= 0 && triangle[1].normal >= 0 &&
            triangle[2].normal >= 0)
        {
            continue;
        }

        anyMissingNormal = true;
        const Float3& a = positions[static_cast<std::size_t>(triangle[0].position)];
        const Float3& b = positions[static_cast<std::size_t>(triangle[1].position)];
        const Float3& c = positions[static_cast<std::size_t>(triangle[2].position)];
        // Unnormalised cross product = area-weighted face normal.
        const Float3 face = Cross(
            {b.x - a.x, b.y - a.y, b.z - a.z},
            {c.x - a.x, c.y - a.y, c.z - a.z});

        for (const CornerKey& corner : triangle)
        {
            Float3& target = smooth[static_cast<std::size_t>(corner.position)];
            target.x += face.x;
            target.y += face.y;
            target.z += face.z;
        }
    }

    // Bounds over the positions actually used.
    Float3 low{
        std::numeric_limits<f32>::max(),
        std::numeric_limits<f32>::max(),
        std::numeric_limits<f32>::max()};
    Float3 high{
        std::numeric_limits<f32>::lowest(),
        std::numeric_limits<f32>::lowest(),
        std::numeric_limits<f32>::lowest()};

    for (const auto& triangle : triangles)
    {
        for (const CornerKey& corner : triangle)
        {
            const Float3& p = positions[static_cast<std::size_t>(corner.position)];
            low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        }
    }

    const Float3 centre{
        (low.x + high.x) * 0.5F,
        (low.y + high.y) * 0.5F,
        (low.z + high.z) * 0.5F};

    f32 radius = 0.0F;
    for (const auto& triangle : triangles)
    {
        for (const CornerKey& corner : triangle)
        {
            const Float3& p = positions[static_cast<std::size_t>(corner.position)];
            radius = std::max(
                radius,
                std::sqrt(
                    (p.x - centre.x) * (p.x - centre.x) +
                    (p.y - centre.y) * (p.y - centre.y) +
                    (p.z - centre.z) * (p.z - centre.z)));
        }
    }

    const f32 scale = radius > 1.0e-20F ? 1.0F / radius : 1.0F;

    MeshData mesh;
    mesh.sourceMin[0] = low.x;
    mesh.sourceMin[1] = low.y;
    mesh.sourceMin[2] = low.z;
    mesh.sourceMax[0] = high.x;
    mesh.sourceMax[1] = high.y;
    mesh.sourceMax[2] = high.z;
    mesh.sourceRadius = radius;
    mesh.hadNormals = !normals.empty() && !anyMissingNormal;
    mesh.hadUvs = !uvs.empty();
    mesh.sourceFaces = faceCount;

    std::unordered_map<CornerKey, u32, CornerHash> lookup;
    mesh.indices.reserve(triangles.size() * 3U);

    for (const auto& triangle : triangles)
    {
        for (const CornerKey& corner : triangle)
        {
            const auto [slot, inserted] = lookup.try_emplace(
                corner, static_cast<u32>(mesh.vertices.size()));

            if (inserted)
            {
                const Float3& p =
                    positions[static_cast<std::size_t>(corner.position)];
                const Float3 fitted{
                    (p.x - centre.x) * scale,
                    (p.y - centre.y) * scale,
                    (p.z - centre.z) * scale};

                Float3 n = corner.normal >= 0
                    ? normals[static_cast<std::size_t>(corner.normal)]
                    : Normalise(smooth[static_cast<std::size_t>(corner.position)]);

                MeshVertex vertex;
                vertex.position[0] = fitted.x;
                vertex.position[1] = fitted.y;
                vertex.position[2] = fitted.z;
                vertex.normal[0] = n.x;
                vertex.normal[1] = n.y;
                vertex.normal[2] = n.z;

                if (corner.uv >= 0)
                {
                    const auto& uv = uvs[static_cast<std::size_t>(corner.uv)];
                    vertex.uv[0] = uv[0];
                    vertex.uv[1] = uv[1];
                }
                else
                {
                    // Spherical projection about the model's centre.
                    const Float3 d = Normalise(fitted);
                    vertex.uv[0] =
                        0.5F + std::atan2(d.x, d.z) / (2.0F * std::numbers::pi_v<f32>);
                    vertex.uv[1] =
                        0.5F - std::asin(std::clamp(d.y, -1.0F, 1.0F)) /
                            std::numbers::pi_v<f32>;
                }

                mesh.vertices.push_back(vertex);
            }

            mesh.indices.push_back(slot->second);
        }
    }

    return mesh;
}
} // namespace orbit::shading
