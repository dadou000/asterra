#include <orbit/lighting/TerrainSunShadow.hpp>
#include <orbit/celestial_lighting/CelestialLighting.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
constexpr f64 kPi = std::numbers::pi;

f64 Elevation(const TerrainSunShadowField& f, const math::Double3 p)
{
    const auto d = math::Normalize(p);
    const f64 x = (std::atan2(d.z, d.x) / (2.0 * kPi) + 0.5) * f.width - 0.5;
    const f64 y = std::clamp((std::asin(std::clamp(d.y, -1.0, 1.0)) / kPi + 0.5) *
        f.height - 0.5, 0.0, static_cast<f64>(f.height - 1U));
    const auto ix = static_cast<i32>(std::floor(x));
    const auto iy = static_cast<u32>(std::floor(y));
    const auto read = [&](i32 cx, u32 cy)
    {
        const auto w = static_cast<i32>(f.width);
        cx = (cx % w + w) % w;
        return static_cast<f64>(f.elevations[cy * f.width + static_cast<u32>(cx)]);
    };
    const f64 tx = x - std::floor(x), ty = y - iy;
    return std::lerp(std::lerp(read(ix, iy), read(ix + 1, iy), tx),
        std::lerp(read(ix, std::min(iy + 1U, f.height - 1U)),
                  read(ix + 1, std::min(iy + 1U, f.height - 1U)), tx), ty);
}
}

TerrainSunShadowField BuildTerrainSunShadowField(const terrain::TerrainSource& source,
    const f64 radiusMeters, const u32 width, const u32 height)
{
    if (!std::isfinite(radiusMeters) || radiusMeters <= 0.0 || width < 4U || height < 2U)
        throw std::invalid_argument("Invalid terrain sun shadow field dimensions/radius.");
    TerrainSunShadowField field;
    field.width = width;
    field.height = height;
    field.radiusMeters = radiusMeters;
    field.spacingMeters = kPi * radiusMeters / height;
    field.sourceRevision = source.Revision();
    field.minimumElevationMeters = std::numeric_limits<f64>::infinity();
    field.maximumElevationMeters = -std::numeric_limits<f64>::infinity();
    field.elevations.resize(static_cast<std::size_t>(width) * height);
    for (u32 y = 0; y < height; ++y)
    {
        const f64 latitude = ((static_cast<f64>(y) + 0.5) / height - 0.5) * kPi;
        for (u32 x = 0; x < width; ++x)
        {
            const f64 longitude = ((static_cast<f64>(x) + 0.5) / width - 0.5) * (2.0 * kPi);
            const auto sample = source.Sample({
                .unitDirection = {std::cos(latitude) * std::cos(longitude),
                    std::sin(latitude), std::cos(latitude) * std::sin(longitude)},
                .footprintMeters = field.spacingMeters});
            const f64 elevation = sample.elevationMeters; // Standing water is not opaque relief.
            if (!std::isfinite(elevation) || radiusMeters + elevation <= 0.0)
                throw std::runtime_error("Terrain sun shadow candidate contains invalid elevations.");
            field.elevations[static_cast<std::size_t>(y) * width + x] = static_cast<f32>(elevation);
            field.minimumElevationMeters = std::min(field.minimumElevationMeters, elevation);
            field.maximumElevationMeters = std::max(field.maximumElevationMeters, elevation);
        }
    }
    if (source.Revision() != field.sourceRevision)
        throw std::runtime_error("Terrain changed during sun shadow candidate generation.");
    return field;
}

f64 TerrainSunVisibility(const TerrainSunShadowField& field, const math::Double3 position,
    math::Double3 sun, const f64 angularRadius)
{
    sun = math::Normalize(sun);
    const f64 along = math::Dot(position, sun);
    const auto perpendicular = position - sun * along;
    const f64 perpendicular2 = math::LengthSquared(perpendicular);
    const f64 bias = std::max(0.5, field.radiusMeters * 2.0e-7);
    const f64 inner = field.radiusMeters + field.minimumElevationMeters - bias;
    f64 visibility = 1.0;
    if (angularRadius > 0.0)
    {
        constexpr f64 sourceDistance = 1.0e15;
        visibility = celestial_lighting::FiniteDiscOccultation(
            {.centerFromObserverMeters = sun * sourceDistance, .radiusMeters = sourceDistance * std::sin(angularRadius)},
            {.centerFromObserverMeters = position * -1.0, .radiusMeters = inner}).visibleFraction;
        if (visibility <= 0.0) return 0.0;
    }
    else if (along < 0.0 && perpendicular2 < inner * inner) return 0.0;
    const f64 outer = field.radiusMeters + field.maximumElevationMeters + bias +
        math::Length(position) * std::sin(angularRadius);
    const f64 discriminant = outer * outer - perpendicular2;
    if (discriminant <= 0.0) return visibility;
    const f64 root = std::sqrt(discriminant);
    const f64 start = std::max(-along - root, 0.0), end = -along + root;
    if (end <= start) return visibility;
    const f64 step = std::max(field.spacingMeters * 0.5, (end - start) / 510.0);
    // Anchor sun-ray samples to the body origin, independent of the camera.
    f64 t = std::max(start, std::ceil((along + start) / step) * step - along);
    for (u32 i = 0; i < 512U && t <= end; ++i, t += step)
    {
        const auto p = position + sun * t;
        const f64 clearance = math::Length(p) - field.radiusMeters - Elevation(field, p) + bias;
        if (angularRadius <= 0.0)
        {
            if (clearance < 0.0) return 0.0;
        }
        else if (t > bias * 4.0)
        {
            visibility = std::min(visibility, celestial_lighting::DiscHorizonVisibleFraction(
                std::atan2(clearance, t), angularRadius));
            if (visibility <= 0.0) return 0.0;
        }
    }
    return visibility;
}

std::shared_ptr<rhi::Buffer> UploadTerrainSunShadowField(rhi::Device& device,
    const TerrainSunShadowField& field)
{
    const std::array<u32, 8> header{
        std::bit_cast<u32>(static_cast<f32>(field.radiusMeters)),
        std::bit_cast<u32>(static_cast<f32>(field.minimumElevationMeters)),
        std::bit_cast<u32>(static_cast<f32>(field.maximumElevationMeters)), field.width,
        field.height, std::bit_cast<u32>(static_cast<f32>(field.spacingMeters)), 0U, 0U};
    auto buffer = device.CreateBuffer({
        .sizeBytes = sizeof(header) + field.elevations.size() * sizeof(f32),
        .usage = rhi::BufferUsage::Structured, .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    auto* bytes = static_cast<u8*>(buffer->Map());
    std::memcpy(bytes, header.data(), sizeof(header));
    std::memcpy(bytes + sizeof(header), field.elevations.data(), field.elevations.size() * sizeof(f32));
    buffer->Unmap();
    return std::shared_ptr<rhi::Buffer>(std::move(buffer));
}

std::unique_ptr<rhi::Buffer> CreateEmptyTerrainSunShadowBuffer(rhi::Device& device)
{
    auto buffer = device.CreateBuffer({.sizeBytes = 64U, .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible, .initialState = rhi::ResourceState::ShaderResource});
    std::memset(buffer->Map(), 0, 64U);
    buffer->Unmap();
    return buffer;
}
} // namespace orbit::lighting
