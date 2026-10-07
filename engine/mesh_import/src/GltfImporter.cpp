#include <orbit/mesh_import/GltfImporter.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit::mesh_import
{
namespace
{
using Json = nlohmann::json;

constexpr u32 kGlbMagic = 0x46546C67U;     // "glTF"
constexpr u32 kChunkJson = 0x4E4F534AU;    // "JSON"
constexpr u32 kChunkBinary = 0x004E4942U;  // "BIN\0"
constexpr u32 kMaximumNodeDepth = 256U;

constexpr i32 kByte = 5120;
constexpr i32 kUnsignedByte = 5121;
constexpr i32 kShort = 5122;
constexpr i32 kUnsignedShort = 5123;
constexpr i32 kUnsignedInt = 5125;
constexpr i32 kFloat = 5126;

[[noreturn]] void Fail(const std::string& message)
{
    throw MeshImportError("glTF import: " + message);
}

// 4x4 column-major affine transform in double precision.
using Mat4 = std::array<f64, 16>;

[[nodiscard]] Mat4 Identity() noexcept
{
    return {
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0};
}

[[nodiscard]] Mat4 Multiply(const Mat4& a, const Mat4& b) noexcept
{
    Mat4 r{};
    for (std::size_t column = 0U; column < 4U; ++column)
    {
        for (std::size_t row = 0U; row < 4U; ++row)
        {
            f64 sum = 0.0;
            for (std::size_t k = 0U; k < 4U; ++k)
            {
                sum += a[k * 4U + row] * b[column * 4U + k];
            }
            r[column * 4U + row] = sum;
        }
    }
    return r;
}

[[nodiscard]] Mat4 FromTrs(
    const std::array<f64, 3>& t,
    const std::array<f64, 4>& q,
    const std::array<f64, 3>& s) noexcept
{
    f64 x = q[0];
    f64 y = q[1];
    f64 z = q[2];
    f64 w = q[3];
    const f64 length = std::sqrt(x * x + y * y + z * z + w * w);
    if (length > 0.0)
    {
        x /= length;
        y /= length;
        z /= length;
        w /= length;
    }
    else
    {
        x = y = z = 0.0;
        w = 1.0;
    }

    Mat4 m = Identity();
    m[0] = (1.0 - 2.0 * (y * y + z * z)) * s[0];
    m[1] = (2.0 * (x * y + z * w)) * s[0];
    m[2] = (2.0 * (x * z - y * w)) * s[0];
    m[4] = (2.0 * (x * y - z * w)) * s[1];
    m[5] = (1.0 - 2.0 * (x * x + z * z)) * s[1];
    m[6] = (2.0 * (y * z + x * w)) * s[1];
    m[8] = (2.0 * (x * z + y * w)) * s[2];
    m[9] = (2.0 * (y * z - x * w)) * s[2];
    m[10] = (1.0 - 2.0 * (x * x + y * y)) * s[2];
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    return m;
}

struct Float3
{
    f64 x{0.0};
    f64 y{0.0};
    f64 z{0.0};
};

[[nodiscard]] Float3 Sub(const Float3& a, const Float3& b) noexcept
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] Float3 Cross(const Float3& a, const Float3& b) noexcept
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}

[[nodiscard]] f64 Dot(const Float3& a, const Float3& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] Float3 Normalized(const Float3& v, const Float3& fallback) noexcept
{
    const f64 length = std::sqrt(Dot(v, v));
    if (!(length > 1.0e-20) || !std::isfinite(length))
    {
        return fallback;
    }
    return {v.x / length, v.y / length, v.z / length};
}

// Upper-left 3x3 as the cofactor matrix: equals det * inverse-transpose, so
// it transforms normals correctly under non-uniform scale. The sign of the
// determinant is returned so mirrored transforms can restore the direction.
struct NormalMatrix
{
    std::array<f64, 9> c{}; // row-major cofactor
    f64 determinantSign{1.0};
};

[[nodiscard]] NormalMatrix MakeNormalMatrix(const Mat4& m) noexcept
{
    // m is column-major: element (row, column) = m[column * 4 + row].
    const auto e = [&m](std::size_t row, std::size_t column)
    {
        return m[column * 4U + row];
    };

    NormalMatrix n;
    n.c[0] = e(1, 1) * e(2, 2) - e(1, 2) * e(2, 1);
    n.c[1] = e(1, 2) * e(2, 0) - e(1, 0) * e(2, 2);
    n.c[2] = e(1, 0) * e(2, 1) - e(1, 1) * e(2, 0);
    n.c[3] = e(0, 2) * e(2, 1) - e(0, 1) * e(2, 2);
    n.c[4] = e(0, 0) * e(2, 2) - e(0, 2) * e(2, 0);
    n.c[5] = e(0, 1) * e(2, 0) - e(0, 0) * e(2, 1);
    n.c[6] = e(0, 1) * e(1, 2) - e(0, 2) * e(1, 1);
    n.c[7] = e(0, 2) * e(1, 0) - e(0, 0) * e(1, 2);
    n.c[8] = e(0, 0) * e(1, 1) - e(0, 1) * e(1, 0);

    const f64 determinant =
        e(0, 0) * n.c[0] + e(0, 1) * n.c[1] + e(0, 2) * n.c[2];
    n.determinantSign = determinant < 0.0 ? -1.0 : 1.0;
    return n;
}

[[nodiscard]] Float3 TransformPoint(const Mat4& m, const Float3& p) noexcept
{
    return {
        m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
        m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
        m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}

[[nodiscard]] Float3 TransformDirection(const Mat4& m, const Float3& d) noexcept
{
    return {
        m[0] * d.x + m[4] * d.y + m[8] * d.z,
        m[1] * d.x + m[5] * d.y + m[9] * d.z,
        m[2] * d.x + m[6] * d.y + m[10] * d.z};
}

[[nodiscard]] Float3 TransformNormal(
    const NormalMatrix& n,
    const Float3& v) noexcept
{
    return {
        (n.c[0] * v.x + n.c[1] * v.y + n.c[2] * v.z) * n.determinantSign,
        (n.c[3] * v.x + n.c[4] * v.y + n.c[5] * v.z) * n.determinantSign,
        (n.c[6] * v.x + n.c[7] * v.y + n.c[8] * v.z) * n.determinantSign};
}

// ---- encoding helpers ------------------------------------------------------

[[nodiscard]] std::vector<std::byte> Base64Decode(const std::string_view text)
{
    std::vector<std::byte> out;
    out.reserve(text.size() / 4U * 3U);

    u32 accumulator = 0U;
    i32 bits = -8;
    for (const char character : text)
    {
        i32 value = -1;
        if (character >= 'A' && character <= 'Z')
        {
            value = character - 'A';
        }
        else if (character >= 'a' && character <= 'z')
        {
            value = character - 'a' + 26;
        }
        else if (character >= '0' && character <= '9')
        {
            value = character - '0' + 52;
        }
        else if (character == '+' || character == '-')
        {
            value = 62;
        }
        else if (character == '/' || character == '_')
        {
            value = 63;
        }
        else if (character == '=' || character == '\r' || character == '\n')
        {
            continue;
        }
        else
        {
            Fail("invalid base64 data URI");
        }

        accumulator = (accumulator << 6U) | static_cast<u32>(value);
        bits += 6;
        if (bits >= 0)
        {
            out.push_back(static_cast<std::byte>(
                (accumulator >> static_cast<u32>(bits)) & 0xFFU));
            bits -= 8;
        }
    }
    return out;
}

[[nodiscard]] std::string PercentDecode(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0U; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2U < text.size() &&
            std::isxdigit(static_cast<unsigned char>(text[i + 1U])) != 0 &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2U])) != 0)
        {
            const std::string hex = text.substr(i + 1U, 2U);
            out.push_back(static_cast<char>(std::stoi(hex, nullptr, 16)));
            i += 2U;
        }
        else
        {
            out.push_back(text[i]);
        }
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> ReadFileBytes(
    const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        Fail("cannot open " + path.string());
    }
    const auto size = static_cast<std::size_t>(file.tellg());
    file.seekg(0);
    std::vector<std::byte> bytes(size);
    if (size > 0U &&
        !file.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(size)))
    {
        Fail("cannot read " + path.string());
    }
    return bytes;
}

// Resolves a buffer/image URI: data: URIs and files relative to baseDirectory.
[[nodiscard]] std::vector<std::byte> LoadUri(
    const std::string& uri,
    const std::filesystem::path& baseDirectory)
{
    if (uri.rfind("data:", 0U) == 0U)
    {
        const auto comma = uri.find(',');
        if (comma == std::string::npos)
        {
            Fail("malformed data URI");
        }
        if (uri.substr(0U, comma).find(";base64") == std::string::npos)
        {
            Fail("only base64 data URIs are supported");
        }
        return Base64Decode(std::string_view(uri).substr(comma + 1U));
    }

    if (uri.find("://") != std::string::npos)
    {
        Fail("remote URI is not supported: " + uri);
    }

    const std::string decoded = PercentDecode(uri);
    const std::filesystem::path relative{
        std::u8string(decoded.begin(), decoded.end())};
    return ReadFileBytes(baseDirectory / relative);
}

// ---- JSON accessors --------------------------------------------------------

template <typename T>
[[nodiscard]] T Value(const Json& object, const char* key, const T& fallback)
{
    const auto found = object.find(key);
    if (found == object.end() || found->is_null())
    {
        return fallback;
    }
    try
    {
        return found->get<T>();
    }
    catch (const Json::exception&)
    {
        Fail(std::string("property '") + key + "' has the wrong type");
    }
}

[[nodiscard]] const Json* Child(const Json& object, const char* key)
{
    const auto found = object.find(key);
    return found == object.end() || found->is_null() ? nullptr : &*found;
}

// ---- importer --------------------------------------------------------------

struct Buffers
{
    std::vector<std::vector<std::byte>> data;
};

[[nodiscard]] u32 ComponentSize(const i32 componentType)
{
    switch (componentType)
    {
    case kByte:
    case kUnsignedByte:
        return 1U;
    case kShort:
    case kUnsignedShort:
        return 2U;
    case kUnsignedInt:
    case kFloat:
        return 4U;
    default:
        Fail("unknown accessor component type " +
             std::to_string(componentType));
    }
}

[[nodiscard]] u32 TypeComponents(const std::string& type)
{
    if (type == "SCALAR") { return 1U; }
    if (type == "VEC2") { return 2U; }
    if (type == "VEC3") { return 3U; }
    if (type == "VEC4") { return 4U; }
    if (type == "MAT2") { return 4U; }
    if (type == "MAT3") { return 9U; }
    if (type == "MAT4") { return 16U; }
    Fail("unknown accessor type '" + type + "'");
}

[[nodiscard]] f64 ReadComponent(
    const std::byte* pointer,
    const i32 componentType,
    const bool normalized)
{
    switch (componentType)
    {
    case kByte:
    {
        std::int8_t v;
        std::memcpy(&v, pointer, 1U);
        return normalized ? std::max(static_cast<f64>(v) / 127.0, -1.0)
                          : static_cast<f64>(v);
    }
    case kUnsignedByte:
    {
        std::uint8_t v;
        std::memcpy(&v, pointer, 1U);
        return normalized ? static_cast<f64>(v) / 255.0 : static_cast<f64>(v);
    }
    case kShort:
    {
        std::int16_t v;
        std::memcpy(&v, pointer, 2U);
        return normalized ? std::max(static_cast<f64>(v) / 32767.0, -1.0)
                          : static_cast<f64>(v);
    }
    case kUnsignedShort:
    {
        std::uint16_t v;
        std::memcpy(&v, pointer, 2U);
        return normalized ? static_cast<f64>(v) / 65535.0
                          : static_cast<f64>(v);
    }
    case kUnsignedInt:
    {
        std::uint32_t v;
        std::memcpy(&v, pointer, 4U);
        return static_cast<f64>(v);
    }
    default:
    {
        f32 v;
        std::memcpy(&v, pointer, 4U);
        return static_cast<f64>(v);
    }
    }
}

class Importer
{
public:
    Importer(
        const Json& document,
        std::vector<std::byte> glbBinary,
        std::filesystem::path baseDirectory,
        const ImportOptions& options)
        : doc_(document),
          baseDirectory_(std::move(baseDirectory)),
          options_(options)
    {
        LoadBuffers(std::move(glbBinary));
    }

    [[nodiscard]] MeshAsset Run(const std::string& name)
    {
        asset_.name = name;
        CheckRequiredExtensions();
        ImportImagesAndTextures();
        ImportMaterials();

        buckets_.assign(asset_.materials.size() + 1U, {});

        const Json* nodes = Child(doc_, "nodes");
        const Json* meshes = Child(doc_, "meshes");
        if (meshes == nullptr || !meshes->is_array() || meshes->empty())
        {
            Fail("the file contains no meshes");
        }

        std::vector<u32> roots = SceneRoots(nodes);
        if (nodes == nullptr || !nodes->is_array() || nodes->empty())
        {
            // No node graph: place every mesh at the origin.
            for (u32 mesh = 0U; mesh < meshes->size(); ++mesh)
            {
                EmitMesh(mesh, Identity());
            }
        }
        else
        {
            for (const u32 root : roots)
            {
                Visit(root, Identity(), 0U);
            }
        }

        Finish();
        return std::move(asset_);
    }

private:
    const Json& doc_;
    std::filesystem::path baseDirectory_;
    ImportOptions options_;
    Buffers buffers_;
    MeshAsset asset_;
    // Index lists per material, the last bucket is the implicit default.
    std::vector<std::vector<u32>> buckets_;
    bool defaultMaterialUsed_{false};
    std::set<std::string> reportedWarnings_;
    bool hasBounds_{false};

    void Warn(const std::string& message)
    {
        if (reportedWarnings_.insert(message).second)
        {
            asset_.warnings.push_back(message);
        }
    }

    void LoadBuffers(std::vector<std::byte> glbBinary)
    {
        const Json* buffers = Child(doc_, "buffers");
        if (buffers == nullptr || !buffers->is_array())
        {
            return;
        }

        for (std::size_t i = 0U; i < buffers->size(); ++i)
        {
            const Json& buffer = (*buffers)[i];
            const std::string uri = Value<std::string>(buffer, "uri", {});
            if (uri.empty())
            {
                if (i != 0U || glbBinary.empty())
                {
                    Fail("buffer " + std::to_string(i) +
                         " has no data (missing GLB BIN chunk?)");
                }
                buffers_.data.push_back(std::move(glbBinary));
            }
            else
            {
                buffers_.data.push_back(LoadUri(uri, baseDirectory_));
            }

            const u64 declared = Value<u64>(buffer, "byteLength", 0U);
            if (buffers_.data.back().size() < declared)
            {
                Fail("buffer " + std::to_string(i) + " is shorter than its "
                     "declared byteLength");
            }
        }
    }

    void CheckRequiredExtensions()
    {
        static const std::set<std::string> supported{
            "KHR_mesh_quantization",
            "KHR_materials_emissive_strength",
            "KHR_texture_transform",
            "KHR_materials_unlit"};

        const Json* required = Child(doc_, "extensionsRequired");
        if (required != nullptr && required->is_array())
        {
            for (const Json& extension : *required)
            {
                const std::string id = extension.get<std::string>();
                if (!supported.contains(id))
                {
                    Fail("requires the unsupported extension " + id +
                         " (re-export without Draco/meshopt compression)");
                }
            }
        }
    }

    // Returns {bytes, byteLength, stride} of a buffer view.
    struct ViewRange
    {
        const std::byte* data{nullptr};
        u64 length{0U};
        u64 stride{0U};
    };

    [[nodiscard]] ViewRange View(const u32 index) const
    {
        const Json* views = Child(doc_, "bufferViews");
        if (views == nullptr || index >= views->size())
        {
            Fail("bufferView " + std::to_string(index) + " does not exist");
        }
        const Json& view = (*views)[index];
        const u32 buffer = Value<u32>(view, "buffer", 0U);
        if (buffer >= buffers_.data.size())
        {
            Fail("bufferView " + std::to_string(index) +
                 " references a missing buffer");
        }
        const u64 offset = Value<u64>(view, "byteOffset", 0U);
        const u64 length = Value<u64>(view, "byteLength", 0U);
        const auto& bytes = buffers_.data[buffer];
        if (offset > bytes.size() || length > bytes.size() - offset)
        {
            Fail("bufferView " + std::to_string(index) +
                 " exceeds its buffer");
        }
        return {bytes.data() + offset, length,
                Value<u64>(view, "byteStride", 0U)};
    }

    struct AccessorInfo
    {
        i32 componentType{kFloat};
        u32 components{1U};
        u64 count{0U};
        bool normalized{false};
    };

    [[nodiscard]] AccessorInfo Describe(const u32 index) const
    {
        const Json* accessors = Child(doc_, "accessors");
        if (accessors == nullptr || index >= accessors->size())
        {
            Fail("accessor " + std::to_string(index) + " does not exist");
        }
        const Json& accessor = (*accessors)[index];
        return {
            Value<i32>(accessor, "componentType", kFloat),
            TypeComponents(Value<std::string>(accessor, "type", "SCALAR")),
            Value<u64>(accessor, "count", 0U),
            Value<bool>(accessor, "normalized", false)};
    }

    // Reads an accessor into doubles: count * components values, applying
    // normalisation and sparse substitution.
    [[nodiscard]] std::vector<f64> ReadAccessor(const u32 index) const
    {
        const Json& accessor = (*Child(doc_, "accessors"))[index];
        const AccessorInfo info = Describe(index);
        const u32 size = ComponentSize(info.componentType);
        const u64 elementBytes = static_cast<u64>(size) * info.components;

        std::vector<f64> values(
            static_cast<std::size_t>(info.count) * info.components, 0.0);

        if (const Json* viewIndex = Child(accessor, "bufferView");
            viewIndex != nullptr)
        {
            const ViewRange view = View(viewIndex->get<u32>());
            const u64 base = Value<u64>(accessor, "byteOffset", 0U);
            const u64 stride = view.stride != 0U ? view.stride : elementBytes;
            if (info.count > 0U &&
                base + (info.count - 1U) * stride + elementBytes >
                    view.length)
            {
                Fail("accessor " + std::to_string(index) +
                     " reads past its bufferView");
            }
            for (u64 element = 0U; element < info.count; ++element)
            {
                const std::byte* source = view.data + base + element * stride;
                for (u32 c = 0U; c < info.components; ++c)
                {
                    values[element * info.components + c] = ReadComponent(
                        source + static_cast<std::size_t>(c) * size,
                        info.componentType,
                        info.normalized);
                }
            }
        }

        if (const Json* sparse = Child(accessor, "sparse"); sparse != nullptr)
        {
            const u64 sparseCount = Value<u64>(*sparse, "count", 0U);
            const Json& indicesJson = sparse->at("indices");
            const Json& valuesJson = sparse->at("values");
            const ViewRange indexView =
                View(Value<u32>(indicesJson, "bufferView", 0U));
            const ViewRange valueView =
                View(Value<u32>(valuesJson, "bufferView", 0U));
            const i32 indexType =
                Value<i32>(indicesJson, "componentType", kUnsignedInt);
            const u64 indexBase = Value<u64>(indicesJson, "byteOffset", 0U);
            const u64 valueBase = Value<u64>(valuesJson, "byteOffset", 0U);
            const u32 indexSize = ComponentSize(indexType);

            if (indexBase + sparseCount * indexSize > indexView.length ||
                valueBase + sparseCount * elementBytes > valueView.length)
            {
                Fail("sparse accessor reads past its bufferView");
            }

            for (u64 i = 0U; i < sparseCount; ++i)
            {
                const auto target = static_cast<u64>(ReadComponent(
                    indexView.data + indexBase + i * indexSize,
                    indexType,
                    false));
                if (target >= info.count)
                {
                    Fail("sparse accessor index out of range");
                }
                for (u32 c = 0U; c < info.components; ++c)
                {
                    values[target * info.components + c] = ReadComponent(
                        valueView.data + valueBase + i * elementBytes +
                            static_cast<std::size_t>(c) * size,
                        info.componentType,
                        info.normalized);
                }
            }
        }

        return values;
    }

    // ---- images, textures, materials ---------------------------------------

    void ImportImagesAndTextures()
    {
        if (const Json* images = Child(doc_, "images");
            images != nullptr && images->is_array())
        {
            for (std::size_t i = 0U; i < images->size(); ++i)
            {
                const Json& image = (*images)[i];
                MeshImage out;
                out.name = Value<std::string>(image, "name", {});
                out.mimeType = Value<std::string>(image, "mimeType", {});

                if (const Json* view = Child(image, "bufferView");
                    view != nullptr)
                {
                    const ViewRange range = View(view->get<u32>());
                    out.encoded.assign(
                        range.data, range.data + range.length);
                }
                else if (const std::string uri =
                             Value<std::string>(image, "uri", {});
                         !uri.empty())
                {
                    try
                    {
                        out.encoded = LoadUri(uri, baseDirectory_);
                    }
                    catch (const MeshImportError& error)
                    {
                        Warn(std::string("image ") + std::to_string(i) +
                             " skipped: " + error.what());
                    }
                    if (out.name.empty())
                    {
                        out.name = uri.rfind("data:", 0U) == 0U
                                       ? std::string{}
                                       : uri;
                    }
                }

                if (out.encoded.size() > options_.maximumImageBytes)
                {
                    Warn("image " + std::to_string(i) +
                         " exceeds the size limit and was skipped");
                    out.encoded.clear();
                }
                asset_.images.push_back(std::move(out));
            }
        }

        const Json* samplers = Child(doc_, "samplers");
        if (const Json* textures = Child(doc_, "textures");
            textures != nullptr && textures->is_array())
        {
            for (const Json& texture : *textures)
            {
                MeshTexture out;
                i64 source = Value<i64>(texture, "source", -1);
                if (source < 0)
                {
                    Warn("a texture uses an unsupported image extension "
                         "(KTX2/WebP); it is ignored");
                }
                out.image = static_cast<i32>(source);

                const i64 sampler = Value<i64>(texture, "sampler", -1);
                if (sampler >= 0 && samplers != nullptr &&
                    static_cast<std::size_t>(sampler) < samplers->size())
                {
                    const Json& s = (*samplers)[static_cast<std::size_t>(sampler)];
                    out.repeatU = Value<i32>(s, "wrapS", 10497) != 33071;
                    out.repeatV = Value<i32>(s, "wrapT", 10497) != 33071;
                }
                asset_.textures.push_back(out);
            }
        }
    }

    [[nodiscard]] TextureBinding ReadBinding(
        const Json& parent,
        const char* key,
        const char* scaleKey)
    {
        TextureBinding binding;
        const Json* info = Child(parent, key);
        if (info == nullptr)
        {
            return binding;
        }

        const i32 index = Value<i32>(*info, "index", -1);
        if (index < 0 ||
            static_cast<std::size_t>(index) >= asset_.textures.size() ||
            asset_.textures[static_cast<std::size_t>(index)].image < 0)
        {
            return binding;
        }

        binding.texture = index;
        binding.uvSet = Value<u32>(*info, "texCoord", 0U);
        if (scaleKey != nullptr)
        {
            binding.scale = Value<f32>(*info, scaleKey, 1.0F);
        }
        if (binding.uvSet != 0U)
        {
            Warn("a material uses TEXCOORD_" + std::to_string(binding.uvSet) +
                 "; only TEXCOORD_0 is rendered");
        }
        if (const Json* extensions = Child(*info, "extensions");
            extensions != nullptr &&
            Child(*extensions, "KHR_texture_transform") != nullptr)
        {
            Warn("KHR_texture_transform is ignored");
        }
        return binding;
    }

    void ImportMaterials()
    {
        const Json* materials = Child(doc_, "materials");
        if (materials == nullptr || !materials->is_array())
        {
            return;
        }

        for (const Json& source : *materials)
        {
            MeshMaterial material;
            material.name = Value<std::string>(source, "name", {});

            if (const Json* pbr = Child(source, "pbrMetallicRoughness");
                pbr != nullptr)
            {
                const auto base =
                    Value<std::vector<f32>>(*pbr, "baseColorFactor", {});
                if (base.size() == 4U)
                {
                    std::copy(base.begin(), base.end(),
                              material.baseColorFactor.begin());
                }
                material.metallicFactor =
                    Value<f32>(*pbr, "metallicFactor", 1.0F);
                material.roughnessFactor =
                    Value<f32>(*pbr, "roughnessFactor", 1.0F);
                material.baseColorTexture =
                    ReadBinding(*pbr, "baseColorTexture", nullptr);
                material.metallicRoughnessTexture =
                    ReadBinding(*pbr, "metallicRoughnessTexture", nullptr);
            }

            material.normalTexture =
                ReadBinding(source, "normalTexture", "scale");
            material.occlusionTexture =
                ReadBinding(source, "occlusionTexture", "strength");
            material.emissiveTexture =
                ReadBinding(source, "emissiveTexture", nullptr);

            const auto emissive =
                Value<std::vector<f32>>(source, "emissiveFactor", {});
            if (emissive.size() == 3U)
            {
                std::copy(emissive.begin(), emissive.end(),
                          material.emissiveFactor.begin());
            }

            if (const Json* extensions = Child(source, "extensions");
                extensions != nullptr)
            {
                if (const Json* strength =
                        Child(*extensions, "KHR_materials_emissive_strength");
                    strength != nullptr)
                {
                    const f32 factor =
                        Value<f32>(*strength, "emissiveStrength", 1.0F);
                    for (f32& channel : material.emissiveFactor)
                    {
                        channel *= factor;
                    }
                }
                for (const auto& [id, ignored] : extensions->items())
                {
                    (void)ignored;
                    if (id != "KHR_materials_emissive_strength")
                    {
                        Warn("material extension " + id + " is ignored");
                    }
                }
            }

            const std::string mode =
                Value<std::string>(source, "alphaMode", "OPAQUE");
            material.alphaMode = mode == "MASK"    ? AlphaMode::Mask
                                 : mode == "BLEND" ? AlphaMode::Blend
                                                   : AlphaMode::Opaque;
            material.alphaCutoff = Value<f32>(source, "alphaCutoff", 0.5F);
            material.doubleSided = Value<bool>(source, "doubleSided", false);
            asset_.materials.push_back(std::move(material));
        }
    }

    // ---- scene graph -------------------------------------------------------

    [[nodiscard]] std::vector<u32> SceneRoots(const Json* nodes) const
    {
        std::vector<u32> roots;
        if (nodes == nullptr || !nodes->is_array())
        {
            return roots;
        }

        const Json* scenes = Child(doc_, "scenes");
        if (scenes != nullptr && scenes->is_array() && !scenes->empty())
        {
            const u32 sceneIndex = Value<u32>(doc_, "scene", 0U);
            const Json& scene =
                (*scenes)[std::min<std::size_t>(sceneIndex, scenes->size() - 1U)];
            roots = Value<std::vector<u32>>(scene, "nodes", {});
            if (!roots.empty())
            {
                return roots;
            }
        }

        std::set<u32> children;
        for (const Json& node : *nodes)
        {
            for (const u32 child : Value<std::vector<u32>>(node, "children", {}))
            {
                children.insert(child);
            }
        }
        for (u32 i = 0U; i < nodes->size(); ++i)
        {
            if (!children.contains(i))
            {
                roots.push_back(i);
            }
        }
        return roots;
    }

    void Visit(const u32 nodeIndex, const Mat4& parent, const u32 depth)
    {
        const Json* nodes = Child(doc_, "nodes");
        if (depth > kMaximumNodeDepth)
        {
            Fail("node hierarchy is deeper than " +
                 std::to_string(kMaximumNodeDepth) + " (cycle?)");
        }
        if (nodeIndex >= nodes->size())
        {
            Fail("node " + std::to_string(nodeIndex) + " does not exist");
        }
        const Json& node = (*nodes)[nodeIndex];

        Mat4 local = Identity();
        if (const auto matrix = Value<std::vector<f64>>(node, "matrix", {});
            matrix.size() == 16U)
        {
            std::copy(matrix.begin(), matrix.end(), local.begin());
        }
        else
        {
            const auto t = Value<std::vector<f64>>(node, "translation", {});
            const auto r = Value<std::vector<f64>>(node, "rotation", {});
            const auto s = Value<std::vector<f64>>(node, "scale", {});
            local = FromTrs(
                t.size() == 3U ? std::array<f64, 3>{t[0], t[1], t[2]}
                               : std::array<f64, 3>{0.0, 0.0, 0.0},
                r.size() == 4U ? std::array<f64, 4>{r[0], r[1], r[2], r[3]}
                               : std::array<f64, 4>{0.0, 0.0, 0.0, 1.0},
                s.size() == 3U ? std::array<f64, 3>{s[0], s[1], s[2]}
                               : std::array<f64, 3>{1.0, 1.0, 1.0});
        }

        const Mat4 world = Multiply(parent, local);

        if (const i64 mesh = Value<i64>(node, "mesh", -1); mesh >= 0)
        {
            EmitMesh(static_cast<u32>(mesh), world);
        }

        for (const u32 child : Value<std::vector<u32>>(node, "children", {}))
        {
            Visit(child, world, depth + 1U);
        }
    }

    // ---- geometry ----------------------------------------------------------

    struct PrimitiveData
    {
        std::vector<Float3> positions;
        std::vector<Float3> normals;
        std::vector<std::array<f64, 4>> tangents;
        std::vector<std::array<f64, 2>> uvs;
        std::vector<u32> indices;
        bool hasNormals{false};
        bool hasTangents{false};
        bool hasUvs{false};
    };

    void EmitMesh(const u32 meshIndex, const Mat4& world)
    {
        const Json* meshes = Child(doc_, "meshes");
        if (meshIndex >= meshes->size())
        {
            Fail("mesh " + std::to_string(meshIndex) + " does not exist");
        }

        const Json* primitives = Child((*meshes)[meshIndex], "primitives");
        if (primitives == nullptr || !primitives->is_array())
        {
            return;
        }
        for (const Json& primitive : *primitives)
        {
            EmitPrimitive(primitive, world);
        }
    }

    [[nodiscard]] static std::vector<u32> ToTriangles(
        const std::vector<u32>& source,
        const i32 mode)
    {
        if (mode == 4)
        {
            std::vector<u32> out = source;
            out.resize(out.size() / 3U * 3U);
            return out;
        }

        std::vector<u32> out;
        if (source.size() < 3U)
        {
            return out;
        }
        if (mode == 5) // strip
        {
            for (std::size_t i = 2U; i < source.size(); ++i)
            {
                const bool even = (i % 2U) == 0U;
                out.push_back(source[even ? i - 2U : i - 1U]);
                out.push_back(source[even ? i - 1U : i - 2U]);
                out.push_back(source[i]);
            }
        }
        else // fan
        {
            for (std::size_t i = 2U; i < source.size(); ++i)
            {
                out.push_back(source[0U]);
                out.push_back(source[i - 1U]);
                out.push_back(source[i]);
            }
        }
        return out;
    }

    void EmitPrimitive(const Json& primitive, const Mat4& world)
    {
        const i32 mode = Value<i32>(primitive, "mode", 4);
        if (mode < 4 || mode > 6)
        {
            Warn("points and lines are not imported");
            return;
        }

        const Json* attributes = Child(primitive, "attributes");
        if (attributes == nullptr || Child(*attributes, "POSITION") == nullptr)
        {
            Warn("a primitive without POSITION was skipped");
            return;
        }
        if (Child(primitive, "extensions") != nullptr &&
            Child(*Child(primitive, "extensions"),
                  "KHR_draco_mesh_compression") != nullptr)
        {
            Fail("Draco-compressed geometry is not supported");
        }

        PrimitiveData data;
        {
            const u32 accessor = Value<u32>(*attributes, "POSITION", 0U);
            const AccessorInfo info = Describe(accessor);
            if (info.components != 3U)
            {
                Warn("a POSITION accessor is not VEC3; primitive skipped");
                return;
            }
            const auto values = ReadAccessor(accessor);
            data.positions.reserve(info.count);
            for (u64 i = 0U; i < info.count; ++i)
            {
                data.positions.push_back(
                    {values[i * 3U], values[i * 3U + 1U], values[i * 3U + 2U]});
            }
        }
        const std::size_t vertexCount = data.positions.size();

        const auto readVec = [&](const char* key, const u32 components,
                                 std::vector<f64>& out)
        {
            const Json* found = Child(*attributes, key);
            if (found == nullptr)
            {
                return false;
            }
            const u32 accessor = found->get<u32>();
            const AccessorInfo info = Describe(accessor);
            if (info.components != components || info.count != vertexCount)
            {
                Warn(std::string(key) + " has an unexpected layout and was "
                     "ignored");
                return false;
            }
            out = ReadAccessor(accessor);
            return true;
        };

        std::vector<f64> raw;
        if (readVec("NORMAL", 3U, raw))
        {
            data.hasNormals = true;
            data.normals.reserve(vertexCount);
            for (std::size_t i = 0U; i < vertexCount; ++i)
            {
                data.normals.push_back(
                    {raw[i * 3U], raw[i * 3U + 1U], raw[i * 3U + 2U]});
            }
        }
        if (readVec("TANGENT", 4U, raw))
        {
            data.hasTangents = true;
            data.tangents.reserve(vertexCount);
            for (std::size_t i = 0U; i < vertexCount; ++i)
            {
                data.tangents.push_back(
                    {raw[i * 4U], raw[i * 4U + 1U], raw[i * 4U + 2U],
                     raw[i * 4U + 3U]});
            }
        }
        if (readVec("TEXCOORD_0", 2U, raw))
        {
            data.hasUvs = true;
            data.uvs.reserve(vertexCount);
            for (std::size_t i = 0U; i < vertexCount; ++i)
            {
                data.uvs.push_back({raw[i * 2U], raw[i * 2U + 1U]});
            }
        }
        if (Child(*attributes, "COLOR_0") != nullptr)
        {
            Warn("vertex colours (COLOR_0) are ignored");
        }

        std::vector<u32> source;
        if (const Json* indexAccessor = Child(primitive, "indices");
            indexAccessor != nullptr)
        {
            const auto values = ReadAccessor(indexAccessor->get<u32>());
            source.reserve(values.size());
            for (const f64 value : values)
            {
                source.push_back(static_cast<u32>(value));
            }
        }
        else
        {
            source.resize(vertexCount);
            for (std::size_t i = 0U; i < vertexCount; ++i)
            {
                source[i] = static_cast<u32>(i);
            }
        }
        data.indices = ToTriangles(source, mode);

        // Drop triangles that reference missing vertices rather than the file.
        std::size_t kept = 0U;
        bool droppedAny = false;
        for (std::size_t i = 0U; i + 2U < data.indices.size(); i += 3U)
        {
            if (data.indices[i] < vertexCount &&
                data.indices[i + 1U] < vertexCount &&
                data.indices[i + 2U] < vertexCount)
            {
                data.indices[kept++] = data.indices[i];
                data.indices[kept++] = data.indices[i + 1U];
                data.indices[kept++] = data.indices[i + 2U];
            }
            else
            {
                droppedAny = true;
            }
        }
        data.indices.resize(kept);
        if (droppedAny)
        {
            Warn("triangles with out-of-range indices were dropped");
        }
        if (data.indices.empty())
        {
            return;
        }

        if (asset_.TriangleCount() + data.indices.size() / 3U >
            options_.maximumTriangles)
        {
            Fail("the mesh exceeds the triangle limit of " +
                 std::to_string(options_.maximumTriangles));
        }

        const i64 materialIndex = Value<i64>(primitive, "material", -1);
        std::size_t bucket = asset_.materials.size();
        if (materialIndex >= 0 &&
            static_cast<std::size_t>(materialIndex) < asset_.materials.size())
        {
            bucket = static_cast<std::size_t>(materialIndex);
        }
        else
        {
            defaultMaterialUsed_ = true;
        }

        const bool wantsTangents =
            bucket < asset_.materials.size() &&
            asset_.materials[bucket].normalTexture.Present();

        FinalizeAndAppend(data, world, bucket, wantsTangents);
    }

    static void GenerateFlatNormals(PrimitiveData& data)
    {
        PrimitiveData flat;
        flat.hasNormals = true;
        flat.hasUvs = data.hasUvs;
        flat.hasTangents = data.hasTangents;
        flat.positions.reserve(data.indices.size());
        flat.normals.reserve(data.indices.size());
        flat.indices.reserve(data.indices.size());

        for (std::size_t i = 0U; i < data.indices.size(); i += 3U)
        {
            const Float3& a = data.positions[data.indices[i]];
            const Float3& b = data.positions[data.indices[i + 1U]];
            const Float3& c = data.positions[data.indices[i + 2U]];
            const Float3 normal =
                Normalized(Cross(Sub(b, a), Sub(c, a)), {0.0, 1.0, 0.0});
            for (std::size_t corner = 0U; corner < 3U; ++corner)
            {
                const u32 source = data.indices[i + corner];
                flat.positions.push_back(data.positions[source]);
                flat.normals.push_back(normal);
                if (data.hasUvs)
                {
                    flat.uvs.push_back(data.uvs[source]);
                }
                if (data.hasTangents)
                {
                    flat.tangents.push_back(data.tangents[source]);
                }
                flat.indices.push_back(static_cast<u32>(flat.indices.size()));
            }
        }
        data = std::move(flat);
    }

    // Per-vertex tangents from the UV gradient, orthogonalised against the
    // normal. Good enough for tiling normal maps; authored tangents win.
    static void GenerateTangents(PrimitiveData& data)
    {
        const std::size_t count = data.positions.size();
        std::vector<Float3> tan(count);
        std::vector<Float3> bitan(count);

        for (std::size_t i = 0U; i + 2U < data.indices.size(); i += 3U)
        {
            const u32 i0 = data.indices[i];
            const u32 i1 = data.indices[i + 1U];
            const u32 i2 = data.indices[i + 2U];
            const Float3 e1 = Sub(data.positions[i1], data.positions[i0]);
            const Float3 e2 = Sub(data.positions[i2], data.positions[i0]);
            const f64 du1 = data.uvs[i1][0] - data.uvs[i0][0];
            const f64 dv1 = data.uvs[i1][1] - data.uvs[i0][1];
            const f64 du2 = data.uvs[i2][0] - data.uvs[i0][0];
            const f64 dv2 = data.uvs[i2][1] - data.uvs[i0][1];
            const f64 determinant = du1 * dv2 - du2 * dv1;
            if (std::abs(determinant) < 1.0e-20)
            {
                continue;
            }
            const f64 r = 1.0 / determinant;
            const Float3 t{
                (e1.x * dv2 - e2.x * dv1) * r,
                (e1.y * dv2 - e2.y * dv1) * r,
                (e1.z * dv2 - e2.z * dv1) * r};
            const Float3 b{
                (e2.x * du1 - e1.x * du2) * r,
                (e2.y * du1 - e1.y * du2) * r,
                (e2.z * du1 - e1.z * du2) * r};
            for (const u32 vertex : {i0, i1, i2})
            {
                tan[vertex] = {tan[vertex].x + t.x, tan[vertex].y + t.y,
                               tan[vertex].z + t.z};
                bitan[vertex] = {bitan[vertex].x + b.x, bitan[vertex].y + b.y,
                                 bitan[vertex].z + b.z};
            }
        }

        data.tangents.assign(count, {1.0, 0.0, 0.0, 1.0});
        for (std::size_t v = 0U; v < count; ++v)
        {
            const Float3& n = data.normals[v];
            const f64 along = Dot(n, tan[v]);
            const Float3 orthogonal{
                tan[v].x - n.x * along,
                tan[v].y - n.y * along,
                tan[v].z - n.z * along};
            Float3 axis = Normalized(orthogonal, {0.0, 0.0, 0.0});
            if (Dot(axis, axis) == 0.0)
            {
                // Degenerate UVs: any perpendicular to the normal.
                const Float3 other =
                    std::abs(n.y) < 0.99 ? Float3{0.0, 1.0, 0.0}
                                          : Float3{1.0, 0.0, 0.0};
                axis = Normalized(Cross(other, n), {1.0, 0.0, 0.0});
            }
            const f64 sign = Dot(Cross(n, axis), bitan[v]) < 0.0 ? -1.0 : 1.0;
            data.tangents[v] = {axis.x, axis.y, axis.z, sign};
        }
        data.hasTangents = true;
    }

    void FinalizeAndAppend(
        PrimitiveData& data,
        const Mat4& world,
        const std::size_t bucket,
        const bool wantsTangents)
    {
        if (!data.hasNormals)
        {
            Warn("a primitive had no normals; flat normals were generated");
            GenerateFlatNormals(data);
        }
        if (wantsTangents && !data.hasTangents && data.hasUvs)
        {
            Warn("a normal-mapped primitive had no tangents; they were "
                 "generated from the UVs");
            GenerateTangents(data);
        }

        const NormalMatrix normalMatrix = MakeNormalMatrix(world);
        const bool mirrored = normalMatrix.determinantSign < 0.0;

        const auto base = static_cast<u32>(asset_.vertices.size());
        asset_.vertices.reserve(asset_.vertices.size() + data.positions.size());

        for (std::size_t i = 0U; i < data.positions.size(); ++i)
        {
            MeshVertex vertex;
            const Float3 position = TransformPoint(world, data.positions[i]);
            const Float3 normal = Normalized(
                TransformNormal(normalMatrix, data.normals[i]),
                {0.0, 1.0, 0.0});

            vertex.position = {static_cast<f32>(position.x),
                               static_cast<f32>(position.y),
                               static_cast<f32>(position.z)};
            vertex.normal = {static_cast<f32>(normal.x),
                             static_cast<f32>(normal.y),
                             static_cast<f32>(normal.z)};

            if (data.hasTangents)
            {
                const auto& t = data.tangents[i];
                const Float3 tangent = Normalized(
                    TransformDirection(world, {t[0], t[1], t[2]}),
                    {1.0, 0.0, 0.0});
                const f64 sign = (t[3] < 0.0 ? -1.0 : 1.0) *
                                 (mirrored ? -1.0 : 1.0);
                vertex.tangent = {static_cast<f32>(tangent.x),
                                  static_cast<f32>(tangent.y),
                                  static_cast<f32>(tangent.z),
                                  static_cast<f32>(sign)};
            }
            if (data.hasUvs)
            {
                vertex.uv = {static_cast<f32>(data.uvs[i][0]),
                             static_cast<f32>(data.uvs[i][1])};
            }

            for (std::size_t axis = 0U; axis < 3U; ++axis)
            {
                const f64 component = static_cast<f64>(vertex.position[axis]);
                if (!hasBounds_)
                {
                    asset_.boundsMin[axis] = component;
                    asset_.boundsMax[axis] = component;
                }
                else
                {
                    asset_.boundsMin[axis] =
                        std::min(asset_.boundsMin[axis], component);
                    asset_.boundsMax[axis] =
                        std::max(asset_.boundsMax[axis], component);
                }
                if (axis == 2U)
                {
                    hasBounds_ = true;
                }
            }
            asset_.vertices.push_back(vertex);
        }

        auto& indices = buckets_[bucket];
        indices.reserve(indices.size() + data.indices.size());
        for (std::size_t i = 0U; i + 2U < data.indices.size(); i += 3U)
        {
            indices.push_back(base + data.indices[i]);
            // A mirrored transform flips the triangle winding.
            indices.push_back(base + data.indices[mirrored ? i + 2U : i + 1U]);
            indices.push_back(base + data.indices[mirrored ? i + 1U : i + 2U]);
        }
    }

    void Finish()
    {
        if (defaultMaterialUsed_)
        {
            MeshMaterial fallback;
            fallback.name = "Default";
            fallback.baseColorFactor = {0.8F, 0.8F, 0.8F, 1.0F};
            fallback.metallicFactor = 0.0F;
            fallback.roughnessFactor = 0.9F;
            asset_.materials.push_back(std::move(fallback));
        }
        else
        {
            buckets_.pop_back();
        }

        for (std::size_t material = 0U; material < buckets_.size(); ++material)
        {
            auto& indices = buckets_[material];
            if (indices.empty())
            {
                continue;
            }
            asset_.parts.push_back({
                .firstIndex = static_cast<u32>(asset_.indices.size()),
                .indexCount = static_cast<u32>(indices.size()),
                .material = static_cast<u32>(material)});
            asset_.indices.insert(
                asset_.indices.end(), indices.begin(), indices.end());
            indices.clear();
            indices.shrink_to_fit();
        }

        if (asset_.indices.empty())
        {
            Fail("the file has no triangle geometry");
        }
    }
};

[[nodiscard]] MeshAsset ImportParsed(
    const Json& document,
    std::vector<std::byte> glbBinary,
    const std::filesystem::path& baseDirectory,
    const ImportOptions& options,
    const std::string& name)
{
    if (const Json* asset = Child(document, "asset");
        asset == nullptr ||
        Value<std::string>(*asset, "version", {}).rfind("2.", 0U) != 0U)
    {
        Fail("not a glTF 2.0 document");
    }

    try
    {
        Importer importer(document, std::move(glbBinary), baseDirectory, options);
        return importer.Run(name);
    }
    catch (const Json::exception& error)
    {
        Fail(std::string("malformed glTF: ") + error.what());
    }
}
} // namespace

MeshAsset ImportGltfMemory(
    const std::span<const std::byte> bytes,
    const std::filesystem::path& baseDirectory,
    const ImportOptions& options)
{
    if (bytes.size() < 12U)
    {
        Fail("file too small");
    }

    const auto readU32 = [&bytes](const std::size_t offset)
    {
        u32 value = 0U;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };

    std::vector<std::byte> binary;
    Json document;

    try
    {
        if (readU32(0U) == kGlbMagic)
        {
            if (readU32(4U) != 2U)
            {
                Fail("only GLB version 2 is supported");
            }
            const u32 total =
                std::min<u32>(readU32(8U), static_cast<u32>(bytes.size()));

            bool haveJson = false;
            std::size_t offset = 12U;
            while (offset + 8U <= total)
            {
                const u32 length = readU32(offset);
                const u32 type = readU32(offset + 4U);
                offset += 8U;
                if (length > total - offset)
                {
                    Fail("GLB chunk exceeds the file");
                }
                if (type == kChunkJson && !haveJson)
                {
                    document = Json::parse(
                        reinterpret_cast<const char*>(bytes.data() + offset),
                        reinterpret_cast<const char*>(bytes.data() + offset +
                                                      length));
                    haveJson = true;
                }
                else if (type == kChunkBinary && binary.empty())
                {
                    binary.assign(
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                        bytes.begin() +
                            static_cast<std::ptrdiff_t>(offset + length));
                }
                // 4-byte aligned chunks.
                offset += (static_cast<std::size_t>(length) + 3U) & ~std::size_t{3U};
            }
            if (!haveJson)
            {
                Fail("GLB has no JSON chunk");
            }
        }
        else
        {
            document = Json::parse(
                reinterpret_cast<const char*>(bytes.data()),
                reinterpret_cast<const char*>(bytes.data() + bytes.size()));
        }
    }
    catch (const Json::exception& error)
    {
        Fail(std::string("invalid JSON: ") + error.what());
    }

    return ImportParsed(
        document, std::move(binary), baseDirectory, options, {});
}

MeshAsset ImportGltfFile(
    const std::filesystem::path& path,
    const ImportOptions& options)
{
    const std::vector<std::byte> bytes = ReadFileBytes(path);
    MeshAsset asset = ImportGltfMemory(bytes, path.parent_path(), options);
    asset.sourcePath = path;
    if (asset.name.empty())
    {
        asset.name = path.stem().string();
    }
    return asset;
}
} // namespace orbit::mesh_import
