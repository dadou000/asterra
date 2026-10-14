#include <orbit/celestial_clouds/UnifiedCloudNoise.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace orbit::celestial_clouds::unified
{
namespace
{
[[nodiscard]] constexpr std::uint32_t Hash(
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint32_t z,
    const std::uint32_t seed) noexcept
{
    std::uint32_t n = (x * 0x9e3779b1U) ^ (y * 0x85ebca6bU) ^
        (z * 0xc2b2ae35U) ^ seed;
    n ^= n >> 16U;
    n *= 0x7feb352dU;
    n ^= n >> 15U;
    n *= 0x846ca68bU;
    return n ^ (n >> 16U);
}

[[nodiscard]] constexpr u32 Wrap(
    const int coordinate, const u32 period) noexcept
{
    const int p = static_cast<int>(period);
    return static_cast<u32>((coordinate % p + p) % p);
}

[[nodiscard]] f32 Random01(
    const u32 x, const u32 y, const u32 z, const u32 seed) noexcept
{
    // Top 24 random bits are exactly representable in float.
    return static_cast<f32>(Hash(x,y,z,seed) >> 8U) *
        (1.0F / 16777216.0F);
}

[[nodiscard]] f32 Cellular(
    const u32 x, const u32 y, const u32 z,
    const u32 cells, const u32 seed) noexcept
{
    constexpr f32 side = static_cast<f32>(kUnifiedNoiseSide);
    const f32 px = (static_cast<f32>(x) + 0.5F) *
        static_cast<f32>(cells) / side;
    const f32 py = (static_cast<f32>(y) + 0.5F) *
        static_cast<f32>(cells) / side;
    const f32 pz = (static_cast<f32>(z) + 0.5F) *
        static_cast<f32>(cells) / side;
    const int ix = static_cast<int>(std::floor(px));
    const int iy = static_cast<int>(std::floor(py));
    const int iz = static_cast<int>(std::floor(pz));
    f32 distanceSquared = std::numeric_limits<f32>::max();
    for (int dz = -1; dz <= 1; ++dz)
    {
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                const int cx = ix + dx;
                const int cy = iy + dy;
                const int cz = iz + dz;
                const u32 wx = Wrap(cx, cells);
                const u32 wy = Wrap(cy, cells);
                const u32 wz = Wrap(cz, cells);
                const f32 fx = static_cast<f32>(cx) +
                    0.15F + 0.7F * Random01(wx,wy,wz,seed);
                const f32 fy = static_cast<f32>(cy) +
                    0.15F + 0.7F * Random01(wx,wy,wz,seed+1U);
                const f32 fz = static_cast<f32>(cz) +
                    0.15F + 0.7F * Random01(wx,wy,wz,seed+2U);
                const f32 xDist = px - fx;
                const f32 yDist = py - fy;
                const f32 zDist = pz - fz;
                const f32 d = xDist*xDist + yDist*yDist + zDist*zDist;
                distanceSquared = std::min(distanceSquared, d);
            }
        }
    }
    // Inverted F1 distance. Normalization below removes percentile bias.
    return 1.0F - std::clamp(
        std::sqrt(distanceSquared) * (1.0F / 1.55F), 0.0F, 1.0F);
}
} // namespace

std::vector<f32> BuildUnifiedCloudNoise()
{
    constexpr std::size_t voxels =
        static_cast<std::size_t>(kUnifiedNoiseSide) *
        kUnifiedNoiseSide * kUnifiedNoiseSide;
    std::vector<f32> values(voxels * kUnifiedNoiseChannels);
    std::array<f32, 4> low{}, high{};
    low.fill(std::numeric_limits<f32>::max());
    high.fill(std::numeric_limits<f32>::lowest());

    for (u32 z = 0U; z < kUnifiedNoiseSide; ++z)
    {
        for (u32 y = 0U; y < kUnifiedNoiseSide; ++y)
        {
            for (u32 x = 0U; x < kUnifiedNoiseSide; ++x)
            {
                const f32 broad = Cellular(x,y,z,4U,0x4a1bU);
                const f32 medium = Cellular(x,y,z,8U,0x1f69U);
                const f32 fine = Cellular(x,y,z,16U,0x92bcU);
                const std::array<f32,4> channels{
                    broad*0.62F + medium*0.28F + fine*0.10F,
                    broad, medium, fine
                };
                const std::size_t offset =
                    ((static_cast<std::size_t>(z) * kUnifiedNoiseSide + y) *
                    kUnifiedNoiseSide + x) * kUnifiedNoiseChannels;
                for (std::size_t k = 0; k < channels.size(); ++k)
                {
                    values[offset+k] = channels[k];
                    low[k] = std::min(low[k],channels[k]);
                    high[k] = std::max(high[k],channels[k]);
                }
            }
        }
    }
    // Dynamic normalization per channel prevents a particular Worley seed
    // from producing an almost-empty or uniformly full volume.
    for (std::size_t index = 0; index < voxels; ++index)
    {
        for (std::size_t channel = 0; channel < 4U; ++channel)
        {
            values[index*4U+channel] = std::clamp(
                (values[index*4U+channel]-low[channel]) /
                std::max(1.0e-6F,high[channel]-low[channel]),
                0.0F,1.0F);
        }
    }
    return values;
}
