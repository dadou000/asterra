#include <orbit/volume_representation/VolumeCache.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <type_traits>

namespace orbit::volume_representation
{
namespace
{
constexpr std::array<u8, 8> kMagic{
    'O','R','B','V','O','L','1',0};
constexpr u32 kMaximumResolution = 512U;

[[nodiscard]] u64 Mix(u64 seed, const u64 value) noexcept
{
    seed ^= value + 0x9e3779b97f4a7c15ULL +
        (seed << 6U) + (seed >> 2U);
    return seed;
}

[[nodiscard]] u64 HashString(
    u64 seed,
    const std::string_view value) noexcept
{
    for (const unsigned char byte : value)
    {
        seed = Mix(seed, static_cast<u64>(byte));
    }
    return Mix(seed, static_cast<u64>(value.size()));
}

[[nodiscard]] u64 HashF32(
    u64 seed,
    const f32 value) noexcept
{
    return Mix(seed, std::bit_cast<u32>(value));
}

[[nodiscard]] u64 HashF64(
    u64 seed,
    const f64 value) noexcept
{
    return Mix(seed, std::bit_cast<u64>(value));
}

[[nodiscard]] u64 HashDouble3(
    u64 seed,
    const math::Double3 value) noexcept
{
    seed = HashF64(seed, value.x);
    seed = HashF64(seed, value.y);
    return HashF64(seed, value.z);
}

[[nodiscard]] u64 PayloadFingerprint(
    const std::span<const f32> density,
    const std::span<const f32> emission) noexcept
{
    u64 hash = 0x4f5242564f4c4348ULL;
    hash = Mix(hash, density.size());
    for (const f32 value : density)
    {
        hash = HashF32(hash, value);
    }
    hash = Mix(hash, emission.size());
    for (const f32 value : emission)
    {
        hash = HashF32(hash, value);
    }
    return hash;
}

[[nodiscard]] f32 Saturate(const f64 value) noexcept
{
    return static_cast<f32>(std::clamp(value, 0.0, 1.0));
}

[[nodiscard]] f32 ProceduralNoise(
    const math::Double3 position) noexcept
{
    const f64 a = std::sin(position.x * 0.071 + position.z * 0.053);
    const f64 b = std::sin(position.y * 0.119 - position.x * 0.037 + 1.71);
    const f64 c = std::cos(position.z * 0.101 + position.y * 0.041 - 0.63);
    return static_cast<f32>(
        std::clamp(0.62 + 0.16 * a + 0.13 * b + 0.09 * c, 0.12, 1.0));
}

struct Sample
{
    f32 density{0.0F};
    f32 emission{0.0F};
};

[[nodiscard]] Sample SamplePreset(
    const world_model::ResolvedVolumeDomain& domain,
    const math::Double3 position) noexcept
{
    const math::Double3 half{
        std::max(std::abs(domain.halfExtentsMeters.x), 1.0e-6),
        std::max(std::abs(domain.halfExtentsMeters.y), 1.0e-6),
        std::max(std::abs(domain.halfExtentsMeters.z), 1.0e-6)};
    const math::Double3 local{
        (position.x - domain.centerMeters.x) / half.x,
        (position.y - domain.centerMeters.y) / half.y,
        (position.z - domain.centerMeters.z) / half.z};
    const f64 axis = std::max({
        std::abs(local.x), std::abs(local.y), std::abs(local.z)});
    if (axis >= 1.0)
    {
        return {};
    }

    const f32 edge = Saturate(1.0 - axis);
    const f32 softEdge = edge * edge * (3.0F - 2.0F * edge);
    const f32 noise = ProceduralNoise(position);
    const f32 height = Saturate(local.y * 0.5 + 0.5);
    Sample sample{};

    if (domain.preset == "Fog")
    {
        sample.density = 0.48F * softEdge * (0.82F + 0.18F * noise);
    }
    else if (domain.preset == "Smoke")
    {
        sample.density = 0.82F * softEdge * noise * (1.08F - 0.36F * height);
    }
    else if (domain.preset == "Fire")
    {
        sample.density = 0.66F * softEdge * noise * (1.15F - 0.45F * height);
        sample.emission = sample.density * (0.65F + 0.75F * height);
    }
    else if (domain.preset == "Dust")
    {
        sample.density = 0.58F * softEdge * (0.68F + 0.32F * noise) *
            (1.12F - 0.28F * height);
    }
    else if (domain.preset == "Snow")
    {
        sample.density = 0.31F * softEdge * (0.72F + 0.28F * noise);
    }
    else if (domain.preset == "Surface Flow")
    {
        const f32 band = std::exp(-static_cast<f32>(std::abs(local.y)) * 8.0F);
        sample.density = 0.62F * softEdge * band * noise;
    }
    else if (domain.preset != "Empty")
    {
        sample.density = 0.42F * softEdge * noise;
    }

    return sample;
}

[[nodiscard]] f32 SourceInfluence(
    const world_model::ResolvedVolumeInput& input,
    const math::Double3 position) noexcept
{
    const math::Double3 delta = position - input.positionMeters;

    switch (input.shape)
    {
    case world_model::VolumeSourceShape::Point:
    case world_model::VolumeSourceShape::Sphere:
    {
        const f64 radius = std::max(input.radiusMeters, 1.0e-6);
        return Saturate(1.0 - math::Length(delta) / radius);
    }
    case world_model::VolumeSourceShape::Box:
    case world_model::VolumeSourceShape::Mesh:
    case world_model::VolumeSourceShape::TerrainPatch:
    case world_model::VolumeSourceShape::Spline:
    {
        const math::Double3 half{
            std::max(std::abs(input.halfExtentsMeters.x), 1.0e-6),
            std::max(std::abs(input.halfExtentsMeters.y), 1.0e-6),
            std::max(std::abs(input.halfExtentsMeters.z), 1.0e-6)};
        const f64 axis = std::max({
            std::abs(delta.x) / half.x,
            std::abs(delta.y) / half.y,
            std::abs(delta.z) / half.z});
        return Saturate(1.0 - axis);
    }
    }

    return 0.0F;
}

[[nodiscard]] Sample SampleAuthoredVolume(
    const world_model::ResolvedVolumeDomain& domain,
    const std::span<const world_model::ResolvedVolumeInput> inputs,
    const math::Double3 position) noexcept
{
    Sample sample = SamplePreset(domain, position);

    for (const auto& input : inputs)
    {
        if (!input.enabled || input.role != world_model::VolumeInputRole::Source)
        {
            continue;
        }

        const f32 influence = SourceInfluence(input, position);
        if (influence <= 0.0F)
        {
            continue;
        }

        const f32 scalar = static_cast<f32>(std::max(input.scalarValue, 0.0));
        if ((input.fieldMask & static_cast<u64>(world_model::VolumeField::Density)) != 0U)
        {
            sample.density += scalar * influence;
        }
        if ((input.fieldMask & static_cast<u64>(world_model::VolumeField::Emission)) != 0U)
        {
            sample.emission += scalar * influence;
        }
    }

    sample.density = std::clamp(sample.density, 0.0F, 16.0F);
    sample.emission = std::clamp(sample.emission, 0.0F, 64.0F);
    return sample;
}

void AppendU32(std::vector<u8>& bytes, const u32 value)
{
    for (u32 shift = 0U; shift < 32U; shift += 8U)
    {
        bytes.push_back(static_cast<u8>((value >> shift) & 0xffU));
    }
}

void AppendU64(std::vector<u8>& bytes, const u64 value)
{
    for (u32 shift = 0U; shift < 64U; shift += 8U)
    {
        bytes.push_back(static_cast<u8>((value >> shift) & 0xffULL));
    }
}

void AppendF32(std::vector<u8>& bytes, const f32 value)
{
    AppendU32(bytes, std::bit_cast<u32>(value));
}

void AppendF64(std::vector<u8>& bytes, const f64 value)
{
    AppendU64(bytes, std::bit_cast<u64>(value));
}

struct Reader
{
    std::span<const u8> bytes;
    std::size_t offset{0U};

    [[nodiscard]] bool ReadU32(u32& value) noexcept
    {
        if (offset + 4U > bytes.size()) return false;
        value = 0U;
        for (u32 i = 0U; i < 4U; ++i)
            value |= static_cast<u32>(bytes[offset++]) << (8U * i);
        return true;
    }

    [[nodiscard]] bool ReadU64(u64& value) noexcept
    {
        if (offset + 8U > bytes.size()) return false;
        value = 0U;
        for (u32 i = 0U; i < 8U; ++i)
            value |= static_cast<u64>(bytes[offset++]) << (8U * i);
        return true;
    }

    [[nodiscard]] bool ReadF32(f32& value) noexcept
    {
        u32 bits = 0U;
        if (!ReadU32(bits)) return false;
        value = std::bit_cast<f32>(bits);
        return true;
    }

    [[nodiscard]] bool ReadF64(f64& value) noexcept
    {
        u64 bits = 0U;
        if (!ReadU64(bits)) return false;
        value = std::bit_cast<f64>(bits);
        return true;
    }
};

[[nodiscard]] f32 SampleChannel(
    const std::span<const f32> values,
    const VolumeCacheDescriptor& descriptor,
    const f64 u,
    const f64 v,
    const f64 w) noexcept
{
    if (values.empty() || descriptor.resolutionX == 0U ||
        descriptor.resolutionY == 0U || descriptor.resolutionZ == 0U)
    {
        return 0.0F;
    }

    const f64 x = std::clamp(u, 0.0, 1.0) * (descriptor.resolutionX - 1U);
    const f64 y = std::clamp(v, 0.0, 1.0) * (descriptor.resolutionY - 1U);
    const f64 z = std::clamp(w, 0.0, 1.0) * (descriptor.resolutionZ - 1U);
    const u32 x0 = static_cast<u32>(std::floor(x));
    const u32 y0 = static_cast<u32>(std::floor(y));
    const u32 z0 = static_cast<u32>(std::floor(z));
    const u32 x1 = std::min(x0 + 1U, descriptor.resolutionX - 1U);
    const u32 y1 = std::min(y0 + 1U, descriptor.resolutionY - 1U);
    const u32 z1 = std::min(z0 + 1U, descriptor.resolutionZ - 1U);
    const f32 tx = static_cast<f32>(x - x0);
    const f32 ty = static_cast<f32>(y - y0);
    const f32 tz = static_cast<f32>(z - z0);

    const auto at = [&](const u32 ix, const u32 iy, const u32 iz) noexcept
    {
        const u64 index = (static_cast<u64>(iz) * descriptor.resolutionY + iy) *
            descriptor.resolutionX + ix;
        return index < values.size() ? values[static_cast<std::size_t>(index)] : 0.0F;
    };
    const auto lerp = [](const f32 a, const f32 b, const f32 t) noexcept
    {
        return a + (b - a) * t;
    };

    const f32 c00 = lerp(at(x0,y0,z0), at(x1,y0,z0), tx);
    const f32 c10 = lerp(at(x0,y1,z0), at(x1,y1,z0), tx);
    const f32 c01 = lerp(at(x0,y0,z1), at(x1,y0,z1), tx);
    const f32 c11 = lerp(at(x0,y1,z1), at(x1,y1,z1), tx);
    return lerp(lerp(c00,c10,ty), lerp(c01,c11,ty), tz);
}
} // namespace

u64 ComputeVolumeAuthoredFingerprint(
    const world_model::ResolvedVolumeDomain& domain,
    const std::span<const world_model::ResolvedVolumeInput> inputs,
    const VolumeCacheBakeSettings& settings) noexcept
{
    u64 hash = 0x4f524249544d3337ULL;
    hash = Mix(hash, domain.object.high);
    hash = Mix(hash, domain.object.low);
    hash = HashString(hash, domain.preset);
    hash = HashDouble3(hash, domain.centerMeters);
    hash = HashDouble3(hash, domain.halfExtentsMeters);
    hash = Mix(hash, domain.fieldMask);
    hash = Mix(hash, domain.resolution);
    hash = Mix(hash, settings.resolution);
    hash = Mix(hash, settings.fieldMask);
    hash = Mix(hash, inputs.size());
    for (const auto& input : inputs)
    {
        hash = Mix(hash, input.object.high);
        hash = Mix(hash, input.object.low);
        hash = Mix(hash, input.fingerprint);
        hash = Mix(hash, static_cast<u64>(input.role));
        hash = Mix(hash, static_cast<u64>(input.kind));
        hash = Mix(hash, static_cast<u64>(input.shape));
        hash = Mix(hash, input.fieldMask);
        hash = HashString(hash, input.asset);
    }
    return hash;
}

VolumeCacheData BakeVolumeCache(
    const world_model::ResolvedVolumeDomain& domain,
    const std::span<const world_model::ResolvedVolumeInput> inputs,
    const VolumeCacheBakeSettings& requested)
{
    VolumeCacheBakeSettings settings = requested;
    settings.resolution = std::clamp(settings.resolution, 4U, kMaximumResolution);
    const u64 supportedMask =
        static_cast<u64>(world_model::VolumeField::Density) |
        static_cast<u64>(world_model::VolumeField::Emission);
    settings.fieldMask &= supportedMask;
    if (settings.fieldMask == 0U)
        settings.fieldMask = static_cast<u64>(world_model::VolumeField::Density);

    VolumeCacheData cache;
    cache.descriptor = {
        .schemaVersion = kVolumeCacheSchemaVersion,
        .volume = domain.object,
        .authoredFingerprint = ComputeVolumeAuthoredFingerprint(domain, inputs, settings),
        .resolutionX = settings.resolution,
        .resolutionY = settings.resolution,
        .resolutionZ = settings.resolution,
        .fieldMask = settings.fieldMask,
        .centerMeters = domain.centerMeters,
        .halfExtentsMeters = domain.halfExtentsMeters};

    const u64 count64 = static_cast<u64>(settings.resolution) * settings.resolution * settings.resolution;
    const std::size_t count = static_cast<std::size_t>(count64);
    if ((settings.fieldMask & static_cast<u64>(world_model::VolumeField::Density)) != 0U)
        cache.density.resize(count, 0.0F);
    if ((settings.fieldMask & static_cast<u64>(world_model::VolumeField::Emission)) != 0U)
        cache.emission.resize(count, 0.0F);

    const math::Double3 minimum = domain.centerMeters - domain.halfExtentsMeters;
    const math::Double3 size = domain.halfExtentsMeters * 2.0;
    for (u32 z = 0U; z < settings.resolution; ++z)
    for (u32 y = 0U; y < settings.resolution; ++y)
    for (u32 x = 0U; x < settings.resolution; ++x)
    {
        const math::Double3 position{
            minimum.x + (static_cast<f64>(x) + 0.5) / settings.resolution * size.x,
            minimum.y + (static_cast<f64>(y) + 0.5) / settings.resolution * size.y,
            minimum.z + (static_cast<f64>(z) + 0.5) / settings.resolution * size.z};
        const Sample sample = SampleAuthoredVolume(domain, inputs, position);
        const std::size_t index = (static_cast<std::size_t>(z) * settings.resolution + y) * settings.resolution + x;
        if (!cache.density.empty()) cache.density[index] = sample.density;
        if (!cache.emission.empty()) cache.emission[index] = sample.emission;
    }

    cache.payloadFingerprint = PayloadFingerprint(cache.density, cache.emission);
    return cache;
}

bool SaveVolumeCache(
    const std::string_view path,
    const VolumeCacheData& cache,
    std::string* error)
{
    if (path.empty())
    {
        if (error) *error = "Cache path is empty.";
        return false;
    }

    const u64 expected = static_cast<u64>(cache.descriptor.resolutionX) *
        cache.descriptor.resolutionY * cache.descriptor.resolutionZ;
    if ((!cache.density.empty() && cache.density.size() != expected) ||
        (!cache.emission.empty() && cache.emission.size() != expected))
    {
        if (error) *error = "Cache channel size does not match its dimensions.";
        return false;
    }

    std::vector<u8> bytes;
    bytes.reserve(128U + (cache.density.size() + cache.emission.size()) * sizeof(f32));
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    AppendU32(bytes, cache.descriptor.schemaVersion);
    AppendU32(bytes, cache.descriptor.resolutionX);
    AppendU32(bytes, cache.descriptor.resolutionY);
    AppendU32(bytes, cache.descriptor.resolutionZ);
    AppendU64(bytes, cache.descriptor.fieldMask);
    AppendU64(bytes, cache.descriptor.volume.high);
    AppendU64(bytes, cache.descriptor.volume.low);
    AppendU64(bytes, cache.descriptor.authoredFingerprint);
    const u64 payload = PayloadFingerprint(cache.density, cache.emission);
    AppendU64(bytes, payload);
    AppendF64(bytes, cache.descriptor.centerMeters.x);
    AppendF64(bytes, cache.descriptor.centerMeters.y);
    AppendF64(bytes, cache.descriptor.centerMeters.z);
    AppendF64(bytes, cache.descriptor.halfExtentsMeters.x);
    AppendF64(bytes, cache.descriptor.halfExtentsMeters.y);
    AppendF64(bytes, cache.descriptor.halfExtentsMeters.z);
    AppendU64(bytes, cache.density.size());
    AppendU64(bytes, cache.emission.size());
    for (const f32 value : cache.density) AppendF32(bytes, value);
    for (const f32 value : cache.emission) AppendF32(bytes, value);

    const std::filesystem::path output{std::string(path)};
    std::error_code ec;
    if (output.has_parent_path())
        std::filesystem::create_directories(output.parent_path(), ec);
    if (ec)
    {
        if (error) *error = "Could not create cache directory: " + ec.message();
        return false;
    }

    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        if (error) *error = "Could not open cache file for writing.";
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream.good())
    {
        if (error) *error = "Failed while writing cache file.";
        return false;
    }
    return true;
}

VolumeCacheLoadResult LoadVolumeCache(const std::string_view path)
{
    VolumeCacheLoadResult result;
    const std::filesystem::path input{std::string(path)};
    std::ifstream stream(input, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        result.message = "Could not open volume cache.";
        return result;
    }
    const auto end = stream.tellg();
    if (end < 0)
    {
        result.message = "Could not determine volume cache size.";
        return result;
    }
    std::vector<u8> bytes(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream.good() && !stream.eof())
    {
        result.message = "Failed while reading volume cache.";
        return result;
    }
    if (bytes.size() < kMagic.size() ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
    {
        result.status = VolumeCacheLoadStatus::InvalidMagic;
        result.message = "Not an Orbit volume cache.";
        return result;
    }

    Reader reader{std::span<const u8>(bytes).subspan(kMagic.size())};
    VolumeCacheData cache;
    u64 densityCount = 0U;
    u64 emissionCount = 0U;
    if (!reader.ReadU32(cache.descriptor.schemaVersion))
    {
        result.status = VolumeCacheLoadStatus::Truncated;
        result.message = "Truncated volume cache header.";
        return result;
    }
    if (cache.descriptor.schemaVersion != kVolumeCacheSchemaVersion)
    {
        result.status = VolumeCacheLoadStatus::UnsupportedVersion;
        result.message = "Unsupported volume cache schema version.";
        return result;
    }
    u64 storedPayload = 0U;
    if (!reader.ReadU32(cache.descriptor.resolutionX) ||
        !reader.ReadU32(cache.descriptor.resolutionY) ||
        !reader.ReadU32(cache.descriptor.resolutionZ) ||
        !reader.ReadU64(cache.descriptor.fieldMask) ||
        !reader.ReadU64(cache.descriptor.volume.high) ||
        !reader.ReadU64(cache.descriptor.volume.low) ||
        !reader.ReadU64(cache.descriptor.authoredFingerprint) ||
        !reader.ReadU64(storedPayload) ||
        !reader.ReadF64(cache.descriptor.centerMeters.x) ||
        !reader.ReadF64(cache.descriptor.centerMeters.y) ||
        !reader.ReadF64(cache.descriptor.centerMeters.z) ||
        !reader.ReadF64(cache.descriptor.halfExtentsMeters.x) ||
        !reader.ReadF64(cache.descriptor.halfExtentsMeters.y) ||
        !reader.ReadF64(cache.descriptor.halfExtentsMeters.z) ||
        !reader.ReadU64(densityCount) ||
        !reader.ReadU64(emissionCount))
    {
        result.status = VolumeCacheLoadStatus::Truncated;
        result.message = "Truncated volume cache header.";
        return result;
    }

    const u32 rx = cache.descriptor.resolutionX;
    const u32 ry = cache.descriptor.resolutionY;
    const u32 rz = cache.descriptor.resolutionZ;
    if (rx == 0U || ry == 0U || rz == 0U ||
        rx > kMaximumResolution || ry > kMaximumResolution || rz > kMaximumResolution)
    {
        result.status = VolumeCacheLoadStatus::InvalidDimensions;
        result.message = "Volume cache dimensions are invalid or exceed the M37 safety limit.";
        return result;
    }
    const u64 expected = static_cast<u64>(rx) * ry * rz;
    if ((densityCount != 0U && densityCount != expected) ||
        (emissionCount != 0U && emissionCount != expected) ||
        densityCount > std::numeric_limits<std::size_t>::max() ||
        emissionCount > std::numeric_limits<std::size_t>::max())
    {
        result.status = VolumeCacheLoadStatus::InvalidDimensions;
        result.message = "Volume cache channel counts do not match its dimensions.";
        return result;
    }

    const u64 valueCount = densityCount + emissionCount;
    if (valueCount > (bytes.size() - kMagic.size() - reader.offset) / sizeof(f32))
    {
        result.status = VolumeCacheLoadStatus::Truncated;
        result.message = "Volume cache payload is truncated.";
        return result;
    }
    cache.density.resize(static_cast<std::size_t>(densityCount));
    cache.emission.resize(static_cast<std::size_t>(emissionCount));
    for (f32& value : cache.density)
        if (!reader.ReadF32(value))
        {
            result.status = VolumeCacheLoadStatus::Truncated;
            result.message = "Volume cache density payload is truncated.";
            return result;
        }
    for (f32& value : cache.emission)
        if (!reader.ReadF32(value))
        {
            result.status = VolumeCacheLoadStatus::Truncated;
            result.message = "Volume cache emission payload is truncated.";
            return result;
        }

    cache.payloadFingerprint = PayloadFingerprint(cache.density, cache.emission);
    if (cache.payloadFingerprint != storedPayload)
    {
        result.status = VolumeCacheLoadStatus::CorruptPayload;
        result.message = "Volume cache checksum does not match its payload.";
        return result;
    }
    cache.sourcePath = std::string(path);
    result.status = VolumeCacheLoadStatus::Ok;
    result.message = "Volume cache imported and validated.";
    result.cache = std::move(cache);
    return result;
}

bool IsVolumeCacheCurrent(
    const VolumeCacheData& cache,
    const world_model::ResolvedVolumeDomain& domain,
    const std::span<const world_model::ResolvedVolumeInput> inputs,
    const VolumeCacheBakeSettings& settings,
    std::string* reason) noexcept
{
    if (cache.descriptor.schemaVersion != kVolumeCacheSchemaVersion)
    {
        if (reason) *reason = "Cache schema is not current.";
        return false;
    }
    const u64 fingerprint = ComputeVolumeAuthoredFingerprint(domain, inputs, settings);
    if (cache.descriptor.authoredFingerprint != 0U &&
        cache.descriptor.authoredFingerprint != fingerprint)
    {
        if (reason) *reason = "Authored volume or bake settings changed since this cache was baked.";
        return false;
    }
    if (cache.payloadFingerprint != PayloadFingerprint(cache.density, cache.emission))
    {
        if (reason) *reason = "Cache payload checksum is invalid.";
        return false;
    }
    return true;
}

f32 SampleVolumeCacheDensity(
    const VolumeCacheData& cache,
    const f64 u,
    const f64 v,
    const f64 w) noexcept
{
    return SampleChannel(cache.density, cache.descriptor, u, v, w);
}

f32 SampleVolumeCacheEmission(
    const VolumeCacheData& cache,
    const f64 u,
    const f64 v,
    const f64 w) noexcept
{
    return SampleChannel(cache.emission, cache.descriptor, u, v, w);
}

void VolumeCacheRegistry::Attach(
    const scene::ObjectId volume,
    VolumeCacheData cache)
{
    cache.descriptor.volume = volume;
    caches_.insert_or_assign(volume, std::move(cache));
}

const VolumeCacheData* VolumeCacheRegistry::Find(
    const scene::ObjectId volume) const noexcept
{
    const auto found = caches_.find(volume);
    return found == caches_.end() ? nullptr : &found->second;
}

bool VolumeCacheRegistry::Has(const scene::ObjectId volume) const noexcept
{
    return caches_.contains(volume);
}

void VolumeCacheRegistry::Detach(const scene::ObjectId volume) noexcept
{
    caches_.erase(volume);
}

void VolumeCacheRegistry::RemoveMissing(const scene::ObjectStore& objects)
{
    std::erase_if(caches_, [&objects](const auto& item)
    {
        const auto record = objects.Find(item.first);
        return !record.has_value() || record->type != world_model::kVolumeType;
    });
}

VolumeCacheRegistry& VolumeCaches() noexcept
{
    static VolumeCacheRegistry registry;
    return registry;
}

std::string_view VolumeCacheLoadStatusName(const VolumeCacheLoadStatus status) noexcept
{
    switch (status)
    {
    case VolumeCacheLoadStatus::Ok: return "Ok";
    case VolumeCacheLoadStatus::IoError: return "I/O error";
    case VolumeCacheLoadStatus::InvalidMagic: return "Invalid magic";
    case VolumeCacheLoadStatus::UnsupportedVersion: return "Unsupported version";
    case VolumeCacheLoadStatus::InvalidDimensions: return "Invalid dimensions";
    case VolumeCacheLoadStatus::Truncated: return "Truncated";
    case VolumeCacheLoadStatus::CorruptPayload: return "Corrupt payload";
    }
    return "Unknown";
}
} // namespace orbit::volume_representation
