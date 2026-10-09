#include <orbit/terrain_bake/PlanetBakeFile.hpp>

#include <orbit/terrain/BakedRivers.hpp>

#include <algorithm>
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
constexpr u32 kGeologyTag = 0x314F4547U;  // "GEO1"

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

    [[nodiscard]] bool GetBytes(std::vector<char>& bytes, const std::size_t count)
    {
        if (count > size_ - offset_) return false;
        bytes.assign(data_ + offset_, data_ + offset_ + count);
        offset_ += count;
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

[[nodiscard]] std::size_t GeologyIndex(
    const u32 resolution,
    const u32 face,
    const i32 x,
    const i32 y) noexcept
{
    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    return (static_cast<std::size_t>(face) * stride +
        static_cast<std::size_t>(y + 1)) * stride +
        static_cast<std::size_t>(x + 1);
}

[[nodiscard]] u32 GeologyTilesAcross(const u32 resolution) noexcept
{
    u32 tiles = 1U;
    while (resolution / (tiles * 2U) >= 64U &&
           resolution % (tiles * 2U) == 0U && tiles < 64U)
        tiles *= 2U;
    return tiles;
}

void WriteGeologyProcess(Writer& writer, const terrain::BakedGeologyProcessTexel& value)
{
    writer.Put(value.excavationDepthMeters);
    writer.Put(value.ejectaThicknessMeters);
    writer.Put(value.debrisField);
    writer.Put(value.rayField);
    writer.Put(value.meltThicknessMeters);
    writer.Put(value.brecciaField);
    writer.Put(value.resurfacedMaterialFraction);
    writer.Put(value.resurfacingThicknessMeters);
    writer.Put(value.microImpactRoughnessMeters);
    writer.Put(value.microImpactCoverage);
    writer.Put(value.excavationCoverage);
    writer.Put(value.formationAgeYears);
    writer.Put(value.exposureAgeYears);
    writer.Put(value.formationAgeOrder);
    writer.Put(value.exposureAgeOrder);
    writer.Put(value.affectingImpacts);
    writer.Put(value.iceDamage);
    writer.Put(value.fractureCoverage);
    writer.Put(value.nearbySegments);
}

[[nodiscard]] bool ReadGeologyProcess(
    Reader& reader,
    terrain::BakedGeologyProcessTexel& value) noexcept
{
    return reader.Get(value.excavationDepthMeters) &&
        reader.Get(value.ejectaThicknessMeters) && reader.Get(value.debrisField) &&
        reader.Get(value.rayField) && reader.Get(value.meltThicknessMeters) &&
        reader.Get(value.brecciaField) && reader.Get(value.resurfacedMaterialFraction) &&
        reader.Get(value.resurfacingThicknessMeters) &&
        reader.Get(value.microImpactRoughnessMeters) && reader.Get(value.microImpactCoverage) &&
        reader.Get(value.excavationCoverage) && reader.Get(value.formationAgeYears) &&
        reader.Get(value.exposureAgeYears) && reader.Get(value.formationAgeOrder) &&
        reader.Get(value.exposureAgeOrder) && reader.Get(value.affectingImpacts) &&
        reader.Get(value.iceDamage) && reader.Get(value.fractureCoverage) &&
        reader.Get(value.nearbySegments);
}

[[nodiscard]] std::vector<char> CompressTileBytes(const std::vector<char>& input)
{
    std::vector<char> output;
    output.reserve(input.size());
    std::size_t cursor = 0U;
    while (cursor < input.size())
    {
        std::size_t zeroRun = 0U;
        while (cursor + zeroRun < input.size() && input[cursor + zeroRun] == 0 && zeroRun < 127U)
            ++zeroRun;
        if (zeroRun >= 3U)
        {
            output.push_back(static_cast<char>(0x80U | static_cast<u8>(zeroRun)));
            cursor += zeroRun;
            continue;
        }
        const std::size_t start = cursor;
        while (cursor < input.size() && cursor - start < 127U)
        {
            std::size_t nextZeroRun = 0U;
            while (cursor + nextZeroRun < input.size() &&
                   input[cursor + nextZeroRun] == 0 && nextZeroRun < 3U)
                ++nextZeroRun;
            if (nextZeroRun >= 3U) break;
            ++cursor;
        }
        const std::size_t length = cursor - start;
        output.push_back(static_cast<char>(static_cast<u8>(length)));
        output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(start),
            input.begin() + static_cast<std::ptrdiff_t>(cursor));
    }
    return output;
}

[[nodiscard]] bool DecompressTileBytes(
    const std::vector<char>& input,
    const std::size_t expectedSize,
    std::vector<char>& output)
{
    output.clear();
    output.reserve(expectedSize);
    std::size_t cursor = 0U;
    while (cursor < input.size())
    {
        const u8 token = static_cast<u8>(input[cursor++]);
        const std::size_t length = token & 0x7FU;
        if (length == 0U || length > expectedSize - output.size()) return false;
        if ((token & 0x80U) != 0U)
        {
            output.insert(output.end(), length, 0);
        }
        else
        {
            if (length > input.size() - cursor) return false;
            output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(cursor),
                input.begin() + static_cast<std::ptrdiff_t>(cursor + length));
            cursor += length;
        }
    }
    return output.size() == expectedSize;
}

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
    w.Put(rivers.IncisionResolution());
    w.PutArray(rivers.IncisionGutter());
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
    // Version 1 files have no incision field; they load as a graph only.
    if (version != 1U && version != terrain::BakedRiverNetwork::kFormatVersion)
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

    u32 incisionResolution = 0;
    std::vector<f32> incision;
    if (version >= 2U)
    {
        if (!r.Get(incisionResolution) || incisionResolution > 4096U)
        {
            return fail("River incision header is invalid.");
        }
        if (incisionResolution != 0U)
        {
            const std::size_t stride = static_cast<std::size_t>(incisionResolution) + 2U;
            if (!r.GetArray(incision, 6U * stride * stride))
            {
                return fail("River incision data is truncated.");
            }
        }
    }

    try
    {
        return std::make_shared<const terrain::BakedRiverNetwork>(
            terrain::BakedRiverNetwork::Build(
                radius, recipeHash, std::move(nodes), std::move(segments),
                incisionResolution, std::move(incision)));
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

[[nodiscard]] std::vector<char> EncodeGeology(
    const terrain::BakedGeologyRasters& geology)
{
    Writer w;
    w.Put(terrain::BakedGeologyRasters::kFormatVersion);
    w.Put(geology.Resolution());
    w.Put(geology.RecipeHash());
    const u32 tilesAcross = GeologyTilesAcross(geology.Resolution());
    const u32 coreResolution = geology.Resolution() / tilesAcross;
    const u32 tileCount = 6U * tilesAcross * tilesAcross;
    w.Put(tilesAcross);
    w.Put(coreResolution);
    w.Put(tileCount);

    const auto& relief = geology.ReliefGutter();
    const auto& ice = geology.IceReliefGutter();
    const auto& process = geology.ProcessGutter();
    for (u32 face = 0U; face < 6U; ++face)
    {
        for (u32 tileY = 0U; tileY < tilesAcross; ++tileY)
        {
            for (u32 tileX = 0U; tileX < tilesAcross; ++tileX)
            {
                Writer tile;
                tile.Put(face);
                tile.Put(tileX);
                tile.Put(tileY);
                tile.Put(coreResolution);
                for (i32 y = -1; y <= static_cast<i32>(coreResolution); ++y)
                {
                    for (i32 x = -1; x <= static_cast<i32>(coreResolution); ++x)
                    {
                        const i32 sourceX = static_cast<i32>(tileX * coreResolution) + x;
                        const i32 sourceY = static_cast<i32>(tileY * coreResolution) + y;
                        tile.Put(relief[GeologyIndex(geology.Resolution(), face, sourceX, sourceY)]);
                        tile.Put(ice[GeologyIndex(geology.Resolution(), face, sourceX, sourceY)]);
                        WriteGeologyProcess(tile, process[GeologyIndex(
                            geology.Resolution(), face, sourceX, sourceY)]);
                    }
                }
                const u64 tileSize = static_cast<u64>(tile.Data().size());
                std::vector<char> compressed = CompressTileBytes(tile.Data());
                const bool useCompressed = compressed.size() < tile.Data().size();
                const u8 encoding = useCompressed ? 1U : 0U;
                const auto& payload = useCompressed ? compressed : tile.Data();
                const u64 payloadSize = static_cast<u64>(payload.size());
                w.Put(encoding);
                w.Put(tileSize);
                w.Put(payloadSize);
                w.Put(Fnv1a(tile.Data()));
                w.PutBytes(payload);
            }
        }
    }
    return w.Data();
}

[[nodiscard]] std::shared_ptr<const terrain::BakedGeologyRasters>
DecodeGeology(const std::vector<char>& payload, std::string* error)
{
    const auto fail = [error](const char* message)
    {
        if (error != nullptr) *error = message;
        return std::shared_ptr<const terrain::BakedGeologyRasters>{};
    };
    Reader r(payload.data(), payload.size());
    u32 version = 0;
    u32 resolution = 0;
    u64 recipeHash = 0;
    if (!r.Get(version) || !r.Get(resolution) || !r.Get(recipeHash))
        return fail("Geology section header is truncated.");
    if (version < 1U || version > terrain::BakedGeologyRasters::kFormatVersion)
        return fail("Geology section has an unsupported version.");
    if (resolution < 2U || resolution > 4096U)
        return fail("Geology section resolution is invalid.");
    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    std::vector<f32> relief;
    std::vector<f32> iceRelief;
    std::vector<terrain::BakedGeologyProcessTexel> processes;
    if (version >= 4U)
    {
        u32 tilesAcross = 0U;
        u32 tileResolution = 0U;
        u32 tileCount = 0U;
        if (!r.Get(tilesAcross) || !r.Get(tileResolution) || !r.Get(tileCount))
            return fail("Geology regional-tile header is truncated.");
        const u32 expectedTilesAcross = GeologyTilesAcross(resolution);
        const u32 expectedTileCount = 6U * expectedTilesAcross * expectedTilesAcross;
        if (tilesAcross != expectedTilesAcross || tileCount != expectedTileCount ||
            tileResolution != resolution / tilesAcross)
            return fail("Geology regional-tile layout is invalid.");

        const std::size_t expected = 6U * stride * stride;
        relief.assign(expected, 0.0F);
        iceRelief.assign(expected, 0.0F);
        processes.resize(expected);
        std::vector<u8> filled(expected, 0U);
        std::vector<u8> seen(tileCount, 0U);
        const std::size_t tileStride = static_cast<std::size_t>(tileResolution) + 2U;
        const std::size_t tileTexels = tileStride * tileStride;
        const std::size_t maxTileBytes = 16U + tileTexels * 92U;
        for (u32 tileIndex = 0U; tileIndex < tileCount; ++tileIndex)
        {
            u8 encoding = 0U;
            u64 tileSize = 0U;
            u64 payloadSize = 0U;
            u64 tileChecksum = 0U;
            if (!r.Get(encoding) || !r.Get(tileSize) || !r.Get(payloadSize) ||
                !r.Get(tileChecksum) || tileSize < 16U || tileSize > maxTileBytes ||
                payloadSize > maxTileBytes * 2U ||
                payloadSize > r.Remaining())
                return fail("Geology regional tile is truncated or oversized.");
            std::vector<char> encodedBytes;
            if (!r.GetBytes(encodedBytes, static_cast<std::size_t>(payloadSize)))
                return fail("Geology regional tile is truncated.");
            std::vector<char> tileBytes;
            if (encoding == 0U && payloadSize == tileSize)
                tileBytes = std::move(encodedBytes);
            else if (encoding == 1U && !DecompressTileBytes(
                         encodedBytes, static_cast<std::size_t>(tileSize), tileBytes))
                return fail("Geology regional tile compression is invalid.");
            else if (encoding > 1U || (payloadSize != tileSize && encoding == 0U))
                return fail("Geology regional tile encoding is unsupported.");
            if (Fnv1a(tileBytes) != tileChecksum)
                return fail("Geology regional tile failed its checksum.");
            Reader tile(tileBytes.data(), tileBytes.size());
            u32 face = 0U;
            u32 tileX = 0U;
            u32 tileY = 0U;
            u32 encodedResolution = 0U;
            if (!tile.Get(face) || !tile.Get(tileX) || !tile.Get(tileY) ||
                !tile.Get(encodedResolution) || face >= 6U ||
                tileX >= tilesAcross || tileY >= tilesAcross ||
                encodedResolution != tileResolution)
                return fail("Geology regional tile identity is invalid.");
            const std::size_t identity =
                (static_cast<std::size_t>(face) * tilesAcross + tileY) * tilesAcross + tileX;
            if (seen[identity] != 0U)
                return fail("Geology regional tile is duplicated.");
            seen[identity] = 1U;
            for (i32 localY = -1; localY <= static_cast<i32>(tileResolution); ++localY)
            {
                for (i32 localX = -1; localX <= static_cast<i32>(tileResolution); ++localX)
                {
                    f32 impactValue = 0.0F;
                    f32 iceValue = 0.0F;
                    terrain::BakedGeologyProcessTexel processValue{};
                    if (!tile.Get(impactValue) || !tile.Get(iceValue) ||
                        !ReadGeologyProcess(tile, processValue))
                        return fail("Geology regional tile payload is truncated.");
                    const i32 sourceX = static_cast<i32>(tileX * tileResolution) + localX;
                    const i32 sourceY = static_cast<i32>(tileY * tileResolution) + localY;
                    if (sourceX < -1 || sourceX > static_cast<i32>(resolution) ||
                        sourceY < -1 || sourceY > static_cast<i32>(resolution))
                        return fail("Geology regional tile halo exceeds the face bounds.");
                    const u32 ownerX = sourceX < 0 ? 0U :
                        sourceX >= static_cast<i32>(resolution)
                            ? tilesAcross - 1U
                            : static_cast<u32>(sourceX) / tileResolution;
                    const u32 ownerY = sourceY < 0 ? 0U :
                        sourceY >= static_cast<i32>(resolution)
                            ? tilesAcross - 1U
                            : static_cast<u32>(sourceY) / tileResolution;
                    if (tileX != ownerX || tileY != ownerY) continue;
                    const std::size_t target = GeologyIndex(resolution, face, sourceX, sourceY);
                    if (filled[target] != 0U)
                        return fail("Geology regional tiles overlap their canonical coverage.");
                    filled[target] = 1U;
                    relief[target] = impactValue;
                    iceRelief[target] = iceValue;
                    processes[target] = processValue;
                }
            }
            if (tile.Remaining() != 0U)
                return fail("Geology regional tile has trailing bytes.");
        }
        if (std::find(seen.begin(), seen.end(), 0U) != seen.end() ||
            std::find(filled.begin(), filled.end(), 0U) != filled.end() ||
            r.Remaining() != 0U)
            return fail("Geology regional tiles do not cover the complete cube sphere.");
        try
        {
            return std::make_shared<const terrain::BakedGeologyRasters>(
                terrain::BakedGeologyRasters::Build(
                    resolution, recipeHash, std::move(relief), std::move(iceRelief),
                    std::move(processes)));
        }
        catch (const std::exception& exception)
        {
            if (error != nullptr) *error = exception.what();
            return nullptr;
        }
    }
    if (!r.GetArray(relief, 6U * stride * stride) ||
        (version >= 2U && !r.GetArray(iceRelief, 6U * stride * stride)) ||
        (version < 3U && r.Remaining() != 0U))
        return fail("Geology section data is truncated or has trailing bytes.");
    if (version >= 3U)
    {
        processes.resize(6U * stride * stride);
        for (auto& value : processes)
        {
            if (!r.Get(value.excavationDepthMeters) || !r.Get(value.ejectaThicknessMeters) ||
                !r.Get(value.debrisField) || !r.Get(value.rayField) ||
                !r.Get(value.meltThicknessMeters) || !r.Get(value.brecciaField) ||
                !r.Get(value.resurfacedMaterialFraction) ||
                !r.Get(value.resurfacingThicknessMeters) ||
                !r.Get(value.microImpactRoughnessMeters) ||
                !r.Get(value.microImpactCoverage) || !r.Get(value.excavationCoverage) ||
                !r.Get(value.formationAgeYears) || !r.Get(value.exposureAgeYears) ||
                !r.Get(value.formationAgeOrder) || !r.Get(value.exposureAgeOrder) ||
                !r.Get(value.affectingImpacts) || !r.Get(value.iceDamage) ||
                !r.Get(value.fractureCoverage) || !r.Get(value.nearbySegments))
                return fail("Geology process channels are truncated.");
        }
        if (r.Remaining() != 0U)
            return fail("Geology section has trailing bytes.");
    }
    try
    {
        return std::make_shared<const terrain::BakedGeologyRasters>(
            terrain::BakedGeologyRasters::Build(
                resolution, recipeHash, std::move(relief), std::move(iceRelief),
                std::move(processes)));
    }
    catch (const std::exception& exception)
    {
        if (error != nullptr) *error = exception.what();
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
    if (contents.geology != nullptr)
        sections.push_back({kGeologyTag, EncodeGeology(*contents.geology)});

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
        else if (tag == kGeologyTag)
        {
            std::string sectionError;
            contents.geology = DecodeGeology(payload, &sectionError);
            if (contents.geology == nullptr)
            {
                if (error != nullptr) *error = sectionError;
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
