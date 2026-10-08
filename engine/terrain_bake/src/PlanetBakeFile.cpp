#include <orbit/terrain_bake/PlanetBakeFile.hpp>

#include <orbit/terrain/BakedRivers.hpp>

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace orbit::terrain_bake
{
namespace
{
constexpr std::array<char, 8> kMagic{
    'O', 'R', 'B', 'I', 'T', 'B', 'A', 'K'};
constexpr u32 kContainerVersion = 1;
constexpr u32 kTectonicTag = 0x54434554U; // "TECT"
constexpr u32 kRiverTag = 0x52564952U;    // "RIVR"

[[nodiscard]] u64 Fnv1a(const std::vector<char>& bytes) noexcept
{
    u64 hash = 0xCBF29CE484222325ULL;
    for (const char c : bytes)
    {
        hash ^= static_cast<u8>(c);
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

class Writer
{
public:
    template <typename T>
    void Put(const T& value)
    {
        const auto* bytes = reinterpret_cast<const char*>(&value);
        data_.insert(data_.end(), bytes, bytes + sizeof(T));
    }

    template <typename T>
    void PutArray(const std::vector<T>& values)
    {
        const auto* bytes = reinterpret_cast<const char*>(values.data());
        data_.insert(data_.end(), bytes, bytes + values.size() * sizeof(T));
    }

    void PutBytes(const std::vector<char>& bytes)
    {
        data_.insert(data_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] const std::vector<char>& Data() const noexcept
    {
        return data_;
    }

private:
    std::vector<char> data_;
};

class Reader
{
public:
    Reader(const char* data, const std::size_t size)
        : data_(data), size_(size)
    {
    }

    template <typename T>
    [[nodiscard]] bool Get(T& value) noexcept
    {
        if (size_ - offset_ < sizeof(T))
        {
            return false;
        }
        std::memcpy(&value, data_ + offset_, sizeof(T));
        offset_ += sizeof(T);
        return true;
    }

    template <typename T>
    [[nodiscard]] bool GetArray(std::vector<T>& values, const std::size_t count)
    {
        if (count > size_ || (size_ - offset_) / sizeof(T) < count)
        {
            return false;
        }
        const std::size_t bytes = count * sizeof(T);
        values.resize(count);
        std::memcpy(values.data(), data_ + offset_, bytes);
        offset_ += bytes;
        return true;
    }

    [[nodiscard]] std::size_t Remaining() const noexcept
    {
        return size_ - offset_;
    }

private:
    const char* data_;
    std::size_t size_;
    std::size_t offset_{0};
};

[[nodiscard]] std::vector<char> EncodeTectonics(
    const terrain::BakedTectonicRasters& bake)
{
    Writer w;
    w.Put(terrain::BakedTectonicRasters::kFormatVersion);
    w.Put(bake.Resolution());
    w.Put(bake.RecipeHash());
    w.Put(bake.PlateCount());
    w.Put(bake.PlateContinentalFlags());
    for (u32 layer = 0; layer < terrain::kBakedTectonicLayerCount; ++layer)
    {
        const auto id = static_cast<terrain::BakedTectonicLayer>(layer);
        w.Put(bake.RangeMinimum(id));
        w.Put(bake.RangeMaximum(id));
    }
    for (u32 layer = 0; layer < terrain::kBakedTectonicLayerCount; ++layer)
    {
        w.PutArray(
            bake.QuantizedLayer(static_cast<terrain::BakedTectonicLayer>(layer)));
    }
    w.PutArray(bake.PlateIds());
    w.PutArray(bake.NeighbourIds());
    return w.Data();
}

[[nodiscard]] std::shared_ptr<const terrain::BakedTectonicRasters>
DecodeTectonics(const std::vector<char>& payload, std::string* error)
{
    const auto fail = [error](const char* message)
    {
        if (error != nullptr)
        {
            *error = message;
        }
        return std::shared_ptr<const terrain::BakedTectonicRasters>{};
    };

    Reader r(payload.data(), payload.size());
    u32 version = 0;
    u32 resolution = 0;
    u64 recipeHash = 0;
    u32 plateCount = 0;
    std::array<u8, terrain::kBakedTectonicMaxPlates> continental{};
    if (!r.Get(version) || !r.Get(resolution) || !r.Get(recipeHash) ||
        !r.Get(plateCount) || !r.Get(continental))
    {
        return fail("Tectonic section header is truncated.");
    }
    if (version != terrain::BakedTectonicRasters::kFormatVersion)
    {
        return fail("Tectonic section has an unsupported version.");
    }
    if (resolution < 2U || resolution > 4096U ||
        plateCount > terrain::kBakedTectonicMaxPlates)
    {
        return fail("Tectonic section header is invalid.");
    }

    std::array<f32, terrain::kBakedTectonicLayerCount> minimum{};
    std::array<f32, terrain::kBakedTectonicLayerCount> maximum{};
    for (u32 layer = 0; layer < terrain::kBakedTectonicLayerCount; ++layer)
    {
        if (!r.Get(minimum[layer]) || !r.Get(maximum[layer]))
        {
            return fail("Tectonic layer ranges are truncated.");
        }
    }

    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    const std::size_t gutterTexels =
        static_cast<std::size_t>(terrain::kBakedTectonicFaces) * stride * stride;
    const std::size_t plateTexels =
        static_cast<std::size_t>(terrain::kBakedTectonicFaces) * resolution *
        resolution;

    std::array<std::vector<u16>, terrain::kBakedTectonicLayerCount> layers;
    for (auto& layer : layers)
    {
        if (!r.GetArray(layer, gutterTexels))
        {
            return fail("Tectonic layer data is truncated.");
        }
    }
    std::vector<u8> plate;
    std::vector<u8> neighbour;
    if (!r.GetArray(plate, plateTexels) || !r.GetArray(neighbour, plateTexels))
    {
        return fail("Tectonic plate data is truncated.");
    }

    try
    {
        return std::make_shared<const terrain::BakedTectonicRasters>(
            terrain::BakedTectonicRasters::FromQuantized(
                resolution,
                recipeHash,
                std::move(layers),
                minimum,
                maximum,
                std::move(plate),
                std::move(neighbour),
                continental,
                plateCount));
    }
    catch (const std::exception& exception)
    {
        if (error != nullptr)
        {
            *error = exception.what();
        }
        return nullptr;
    }
}
[[nodiscard]] std::vector<char> EncodeRivers(const terrain::BakedRiverNetwork& rivers)
{
    Writer w;
    w.Put(terrain::BakedRiverNetwork::kFormatVersion);
    w.Put(rivers.RecipeHash());
    w.Put(rivers.PlanetRadiusMeters());
    w.Put(static_cast<u32>(rivers.Nodes().size()));
    w.Put(static_cast<u32>(rivers.Segments().size()));
    for (const terrain::BakedRiverNode& node : rivers.Nodes())
    {
        w.Put(node.direction.x);
        w.Put(node.direction.y);
        w.Put(node.direction.z);
        w.Put(node.elevationMeters);
        w.Put(node.widthMeters);
        w.Put(node.depthMeters);
        w.Put(node.dischargeCubicMetersPerSecond);
        w.Put(node.basin);
    }
    for (const terrain::BakedRiverSegment& segment : rivers.Segments())
    {
        w.Put(segment.upstream);
        w.Put(segment.downstream);
    }
    return w.Data();
}

[[nodiscard]] std::shared_ptr<const terrain::BakedRiverNetwork>
DecodeRivers(const std::vector<char>& payload, std::string* error)
{
    const auto fail = [error](const char* message)
    {
        if (error != nullptr)
        {
            *error = message;
        }
        return std::shared_ptr<const terrain::BakedRiverNetwork>{};
    };

    Reader r(payload.data(), payload.size());
    u32 version = 0;
    u64 recipeHash = 0;
    f64 radius = 0.0;
    u32 nodeCount = 0;
    u32 segmentCount = 0;
    if (!r.Get(version) || !r.Get(recipeHash) || !r.Get(radius) ||
        !r.Get(nodeCount) || !r.Get(segmentCount))
    {
        return fail("River section header is truncated.");
    }
    if (version != terrain::BakedRiverNetwork::kFormatVersion)
    {
        return fail("River section has an unsupported version.");
    }
    constexpr std::size_t kNodeBytes = 3 * sizeof(f64) + 4 * sizeof(f32) + sizeof(u32);
    if (static_cast<std::size_t>(nodeCount) * kNodeBytes +
            static_cast<std::size_t>(segmentCount) * 2U * sizeof(u32) > r.Remaining())
    {
        return fail("River section data is truncated.");
    }

    std::vector<terrain::BakedRiverNode> nodes(nodeCount);
    for (terrain::BakedRiverNode& node : nodes)
    {
        static_cast<void>(r.Get(node.direction.x));
        static_cast<void>(r.Get(node.direction.y));
        static_cast<void>(r.Get(node.direction.z));
        static_cast<void>(r.Get(node.elevationMeters));
        static_cast<void>(r.Get(node.widthMeters));
        static_cast<void>(r.Get(node.depthMeters));
        static_cast<void>(r.Get(node.dischargeCubicMetersPerSecond));
        static_cast<void>(r.Get(node.basin));
    }
    std::vector<terrain::BakedRiverSegment> segments(segmentCount);
    for (terrain::BakedRiverSegment& segment : segments)
    {
        static_cast<void>(r.Get(segment.upstream));
        static_cast<void>(r.Get(segment.downstream));
    }

    try
    {
        return std::make_shared<const terrain::BakedRiverNetwork>(
            terrain::BakedRiverNetwork::Build(
                radius, recipeHash, std::move(nodes), std::move(segments)));
    }
    catch (const std::exception& exception)
    {
        if (error != nullptr)
        {
            *error = exception.what();
        }
        return nullptr;
    }
}
} // namespace

void SavePlanetBake(
    const std::filesystem::path& path,
    const PlanetBakeContents& contents)
{
    if (contents.tectonics == nullptr)
    {
        throw std::invalid_argument("A planet bake needs at least tectonics.");
    }

    struct Section
    {
        u32 tag;
        std::vector<char> payload;
    };
    std::vector<Section> sections;
    sections.push_back({kTectonicTag, EncodeTectonics(*contents.tectonics)});
    if (contents.rivers != nullptr)
    {
        sections.push_back({kRiverTag, EncodeRivers(*contents.rivers)});
    }

    Writer file;
    for (const char c : kMagic)
    {
        file.Put(c);
    }
    file.Put(kContainerVersion);
    file.Put(static_cast<u32>(sections.size()));
    for (const Section& section : sections)
    {
        file.Put(section.tag);
        file.Put(static_cast<u32>(0));
        file.Put(static_cast<u64>(section.payload.size()));
        file.Put(Fnv1a(section.payload));
        file.PutBytes(section.payload);
    }

    std::error_code ec;
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            throw std::runtime_error("Could not open the bake file for writing.");
        }
        out.write(
            file.Data().data(),
            static_cast<std::streamsize>(file.Data().size()));
        out.flush();
        if (!out)
        {
            throw std::runtime_error("Could not write the bake file.");
        }
    }

    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(temporary, path, ec);
        if (ec)
        {
            throw std::runtime_error(
                "Could not move the bake file into place.");
        }
    }
}

std::optional<PlanetBakeContents> LoadPlanetBake(
    const std::filesystem::path& path,
    std::string* const error)
{
    const auto fail =
        [error](const char* message) -> std::optional<PlanetBakeContents>
    {
        if (error != nullptr)
        {
            *error = message;
        }
        return std::nullopt;
    };

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        return fail("The bake file does not exist.");
    }
    const std::streamsize size = in.tellg();
    if (size < 16)
    {
        return fail("The bake file is truncated.");
    }
    in.seekg(0);
    std::vector<char> bytes(static_cast<std::size_t>(size));
    in.read(bytes.data(), size);
    if (!in)
    {
        return fail("The bake file could not be read.");
    }

    Reader r(bytes.data(), bytes.size());
    std::array<char, 8> magic{};
    u32 version = 0;
    u32 sectionCount = 0;
    if (!r.Get(magic) || magic != kMagic)
    {
        return fail("Not an Orbit bake file.");
    }
    if (!r.Get(version) || version != kContainerVersion)
    {
        return fail("Unsupported bake file version.");
    }
    if (!r.Get(sectionCount) || sectionCount > 64U)
    {
        return fail("The bake file header is invalid.");
    }

    PlanetBakeContents contents;
    for (u32 i = 0; i < sectionCount; ++i)
    {
        u32 tag = 0;
        u32 reserved = 0;
        u64 payloadSize = 0;
        u64 checksum = 0;
        if (!r.Get(tag) || !r.Get(reserved) || !r.Get(payloadSize) ||
            !r.Get(checksum) || payloadSize > r.Remaining())
        {
            return fail("A bake section is truncated.");
        }
        std::vector<char> payload;
        if (!r.GetArray(payload, static_cast<std::size_t>(payloadSize)))
        {
            return fail("A bake section is truncated.");
        }
        if (Fnv1a(payload) != checksum)
        {
            return fail("A bake section failed its checksum.");
        }
        if (tag == kTectonicTag)
        {
            std::string sectionError;
            contents.tectonics = DecodeTectonics(payload, &sectionError);
            if (contents.tectonics == nullptr)
            {
                if (error != nullptr)
                {
                    *error = sectionError;
                }
                return std::nullopt;
            }
        }
        else if (tag == kRiverTag)
        {
            std::string sectionError;
            contents.rivers = DecodeRivers(payload, &sectionError);
            if (contents.rivers == nullptr)
            {
                if (error != nullptr)
                {
                    *error = sectionError;
                }
                return std::nullopt;
            }
        }
    }

    if (contents.tectonics == nullptr)
    {
        return fail("The bake file has no tectonic section.");
    }
    return contents;
}
} // namespace orbit::terrain_bake
