#include <orbit/mesh_import/GltfImporter.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

bool Near(const double a, const double b, const double tolerance = 1.0e-5)
{
    return std::abs(a - b) <= tolerance;
}

std::string Base64(const std::vector<unsigned char>& bytes)
{
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 3)
    {
        const unsigned a = bytes[i];
        const unsigned b = i + 1 < bytes.size() ? bytes[i + 1] : 0U;
        const unsigned c = i + 2 < bytes.size() ? bytes[i + 2] : 0U;
        const unsigned triple = (a << 16U) | (b << 8U) | c;
        out.push_back(table[(triple >> 18U) & 63U]);
        out.push_back(table[(triple >> 12U) & 63U]);
        out.push_back(i + 1 < bytes.size() ? table[(triple >> 6U) & 63U] : '=');
        out.push_back(i + 2 < bytes.size() ? table[triple & 63U] : '=');
    }
    return out;
}

template <typename T>
void Append(std::vector<unsigned char>& bytes, const T& value)
{
    const auto* raw = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(T));
}

// A unit quad in the XY plane facing +Z: positions, normals, uvs, indices.
struct QuadBuffer
{
    std::vector<unsigned char> bytes;
    std::size_t positionOffset{0}, normalOffset{0}, uvOffset{0}, indexOffset{0};
};

QuadBuffer MakeQuad(const bool withNormals = true)
{
    QuadBuffer q;
    q.positionOffset = q.bytes.size();
    const float positions[4][3] = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (const auto& p : positions)
        for (const float c : p)
            Append(q.bytes, c);

    q.normalOffset = q.bytes.size();
    if (withNormals)
        for (int i = 0; i < 4; ++i)
        {
            Append(q.bytes, 0.0F);
            Append(q.bytes, 0.0F);
            Append(q.bytes, 1.0F);
        }

    q.uvOffset = q.bytes.size();
    const float uvs[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (const auto& uv : uvs)
        for (const float c : uv)
            Append(q.bytes, c);

    q.indexOffset = q.bytes.size();
    for (const std::uint16_t index : std::array<std::uint16_t, 6>{0, 1, 2, 0, 2, 3})
        Append(q.bytes, index);
    return q;
}

std::string QuadJson(
    const QuadBuffer& q,
    const std::string& nodeJson,
    const std::string& materialsJson = "[]",
    const std::string& primitiveExtra = "",
    const bool withNormals = true,
    const std::string& topLevelExtra = "")
{
    const std::string normalAttribute =
        withNormals ? ",\"NORMAL\":1" : "";
    const std::string normalView =
        withNormals
            ? "{\"buffer\":0,\"byteOffset\":" +
                  std::to_string(q.normalOffset) + ",\"byteLength\":48},"
            : "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":1},";
    return std::string("{\"asset\":{\"version\":\"2.0\"},") + topLevelExtra +
           "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
           "\"nodes\":[" + nodeJson + "],"
           "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0" +
           normalAttribute + ",\"TEXCOORD_0\":2},\"indices\":3" +
           primitiveExtra + "}]}],"
           "\"materials\":" + materialsJson + ","
           "\"accessors\":["
           "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
           "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
           "{\"bufferView\":2,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
           "{\"bufferView\":3,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],"
           "\"bufferViews\":["
           "{\"buffer\":0,\"byteOffset\":" + std::to_string(q.positionOffset) +
           ",\"byteLength\":48},"
           + normalView +
           "{\"buffer\":0,\"byteOffset\":" + std::to_string(q.uvOffset) +
           ",\"byteLength\":32},"
           "{\"buffer\":0,\"byteOffset\":" + std::to_string(q.indexOffset) +
           ",\"byteLength\":12}],"
           "\"buffers\":[{\"byteLength\":" + std::to_string(q.bytes.size()) +
           ",\"uri\":\"data:application/octet-stream;base64," +
           Base64(q.bytes) + "\"}]}";
}

MeshAsset Import(const std::string& json)
{
    std::vector<std::byte> bytes(json.size());
    std::memcpy(bytes.data(), json.data(), json.size());
    return ImportGltfMemory(bytes);
}

void TestPlainQuad()
{
    const auto asset = Import(QuadJson(MakeQuad(), "{\"mesh\":0}"));
    CHECK(asset.vertices.size() == 4U);
    CHECK(asset.TriangleCount() == 2U);
    CHECK(asset.parts.size() == 1U);
    CHECK(asset.materials.size() == 1U); // implicit default
    CHECK(asset.materials[0].name == "Default");
    CHECK(Near(asset.boundsMax[0], 1.0) && Near(asset.boundsMax[1], 1.0));
    CHECK(Near(asset.vertices[2].uv[0], 1.0) && Near(asset.vertices[2].uv[1], 0.0));
    CHECK(Near(asset.vertices[0].normal[2], 1.0));
}

void TestNodeTransform()
{
    const auto asset = Import(QuadJson(
        MakeQuad(),
        "{\"mesh\":0,\"translation\":[10,0,0],\"scale\":[2,3,4]}"));
    CHECK(Near(asset.boundsMin[0], 10.0));
    CHECK(Near(asset.boundsMax[0], 12.0));
    CHECK(Near(asset.boundsMax[1], 3.0));
    // The normal of a quad scaled (2,3,4) is still +Z.
    CHECK(Near(asset.vertices[0].normal[2], 1.0));
}

void TestMirroredTransformFlipsWinding()
{
    const auto plain = Import(QuadJson(MakeQuad(), "{\"mesh\":0}"));
    const auto mirrored = Import(QuadJson(
        MakeQuad(), "{\"mesh\":0,\"scale\":[-1,1,1]}"));
    CHECK(Near(mirrored.boundsMin[0], -1.0));
    CHECK(plain.indices[1] == 1U && plain.indices[2] == 2U);
    CHECK(mirrored.indices[1] == 2U && mirrored.indices[2] == 1U);
    // Mirroring in X leaves a +Z facing normal alone.
    CHECK(Near(mirrored.vertices[0].normal[2], 1.0));
}

void TestMissingNormalsAreGenerated()
{
    const auto asset = Import(QuadJson(
        MakeQuad(false), "{\"mesh\":0}", "[]", "", false));
    CHECK(asset.vertices.size() == 6U); // unwelded
    CHECK(Near(asset.vertices[0].normal[2], 1.0));
    bool warned = false;
    for (const auto& w : asset.warnings)
        warned = warned || w.find("flat normals") != std::string::npos;
    CHECK(warned);
}

void TestTangentGeneration()
{
    const std::string materials =
        "[{\"normalTexture\":{\"index\":0},\"pbrMetallicRoughness\":{}}]";
    std::string json = QuadJson(
        MakeQuad(), "{\"mesh\":0}", materials, ",\"material\":0");
    // Add a texture + image so the normal map binding resolves.
    json.insert(
        json.size() - 1U,
        ",\"textures\":[{\"source\":0}],"
        "\"images\":[{\"mimeType\":\"image/png\",\"uri\":"
        "\"data:image/png;base64,AAAA\"}]");
    const auto asset = Import(json);
    CHECK(asset.materials.size() == 1U);
    CHECK(asset.materials[0].normalTexture.Present());
    // u increases with +X on the quad, so the generated tangent is +X.
    CHECK(Near(asset.vertices[0].tangent[0], 1.0, 1.0e-4));
    CHECK(Near(asset.vertices[0].tangent[1], 0.0, 1.0e-4));
    CHECK(std::abs(asset.vertices[0].tangent[3]) == 1.0F);
}

void TestOutOfRangeIndicesAreDropped()
{
    auto quad = MakeQuad();
    // Corrupt the last index of the second triangle (3 -> 9).
    const std::uint16_t bad = 9;
    std::memcpy(quad.bytes.data() + quad.indexOffset + 10, &bad, sizeof(bad));
    const auto asset = Import(QuadJson(quad, "{\"mesh\":0}"));
    CHECK(asset.TriangleCount() == 1U);
    CHECK(!asset.warnings.empty());
}

void TestRequiredCompressionIsRefused()
{
    bool threw = false;
    try
    {
        (void)Import(QuadJson(
            MakeQuad(), "{\"mesh\":0}", "[]", "", true,
            "\"extensionsRequired\":[\"KHR_draco_mesh_compression\"],"));
    }
    catch (const MeshImportError& error)
    {
        threw = std::string(error.what()).find("Draco") != std::string::npos;
    }
    CHECK(threw);
}

void TestMaterialsGroupIntoSortedParts()
{
    const std::string materials =
        "[{\"name\":\"a\"},{\"name\":\"b\",\"alphaMode\":\"MASK\","
        "\"alphaCutoff\":0.25,\"doubleSided\":true}]";
    const auto asset = Import(QuadJson(
        MakeQuad(), "{\"mesh\":0}", materials, ",\"material\":1"));
    CHECK(asset.materials.size() == 2U);
    CHECK(asset.parts.size() == 1U);
    CHECK(asset.parts[0].material == 1U);
    CHECK(asset.materials[1].alphaMode == AlphaMode::Mask);
    CHECK(Near(asset.materials[1].alphaCutoff, 0.25));
    CHECK(asset.materials[1].doubleSided);
}

void TestRealFile()
{
#pragma warning(suppress : 4996)
    const char* path = std::getenv("ORBIT_TEST_GLB");
    if (path == nullptr)
        return;
    const auto asset = ImportGltfFile(path);
    std::printf(
        "%s: %zu vertices, %u triangles, %zu parts, %zu materials, "
        "%zu textures, %zu images, bounds (%.1f %.1f %.1f)-(%.1f %.1f %.1f), "
        "%zu warnings\n",
        path, asset.vertices.size(), asset.TriangleCount(), asset.parts.size(),
        asset.materials.size(), asset.textures.size(), asset.images.size(),
        asset.boundsMin[0], asset.boundsMin[1], asset.boundsMin[2],
        asset.boundsMax[0], asset.boundsMax[1], asset.boundsMax[2],
        asset.warnings.size());
    for (const auto& w : asset.warnings)
        std::printf("  warning: %s\n", w.c_str());
    CHECK(asset.TriangleCount() > 0U);
    for (const auto& part : asset.parts)
        CHECK(part.material < asset.materials.size());
}
} // namespace

int main()
{
    TestPlainQuad();
    TestNodeTransform();
    TestMirroredTransformFlipsWinding();
    TestMissingNormalsAreGenerated();
    TestTangentGeneration();
    TestOutOfRangeIndicesAreDropped();
    TestRequiredCompressionIsRefused();
    TestMaterialsGroupIntoSortedParts();
    TestRealFile();
    return failures == 0 ? 0 : 1;
}
