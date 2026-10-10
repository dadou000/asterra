#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace orbit::cloud_genera
{
// The ten WMO cloud genera. One table describes each of them (where it
// lives, how thick it is, how it scatters light), and every renderer reads
// that table: the Weather Lab storm volumes, the generated layer volumes and
// the planet-wide cloud shell all agree on what a "cirrocumulus" is.
enum class CloudGenus : std::uint32_t
{
    Cirrus = 0,
    Cirrocumulus = 1,
    Cirrostratus = 2,
    Altocumulus = 3,
    Altostratus = 4,
    Nimbostratus = 5,
    Stratocumulus = 6,
    Stratus = 7,
    Cumulus = 8,
    Cumulonimbus = 9
};

inline constexpr std::size_t kCloudGenusCount = 10U;

// Level bands (WMO): high ice, middle, low.
enum class CloudLevel : std::uint32_t
{
    High = 0,
    Middle = 1,
    Low = 2,
    Vertical = 3
};

struct CloudGenusInfo
{
    CloudGenus genus{CloudGenus::Cirrus};
    std::string_view name;
    CloudLevel level{CloudLevel::High};
    // Metres above the ground: typical base and top at mid latitudes.
    double baseMeters{0.0};
    double topMeters{0.0};
    // Volume Extinction Scale (1/m per unit baked density): a veil of ice is
    // a thousand times thinner than a storm core.
    double extinctionScale{0.01};
    // 1 for ice crystal clouds (halo, silver edge, no dark base).
    float ice{0.0F};
    // Forward lobe of the Henyey-Greenstein phase function.
    float anisotropy{0.6F};
    // 0 = pitch black base, 1 = no darkening toward the base.
    float baseBrightness{1.0F};
    // Typical size of one cloud element (a cell, a roll, a fibre length), m.
    double elementMeters{1000.0};
    // Half width (m) of a layer volume that resolves the elements.
    double recommendedHalfExtentMeters{64000.0};
    // The Weather Lab simulates it instead of generating it procedurally.
    bool simulated{false};
    // Look variant (0-7, the volume renderer's shader look) and generator style
    // (0-7) chosen by comparing eight alternatives against photographs.
    std::uint32_t lookVariant{0U};
    std::uint32_t defaultStyle{0U};
};

[[nodiscard]] const std::array<CloudGenusInfo, kCloudGenusCount>& CloudGeneraTable() noexcept;
[[nodiscard]] const CloudGenusInfo& GenusInfo(CloudGenus genus) noexcept;

// The number a Volume's Cloud Type property stores: 0 means "decide by height
// and density" (the simulated storm), 1..10 are the genera in table order.
[[nodiscard]] constexpr std::uint32_t CloudTypeCode(const CloudGenus genus) noexcept
{
    return static_cast<std::uint32_t>(genus) + 1U;
}

[[nodiscard]] std::string_view CloudGenusName(CloudGenus genus) noexcept;

// Parses "cirrus", "Altocumulus", ... (case-insensitive); false if unknown.
[[nodiscard]] bool ParseCloudGenus(std::string_view name, CloudGenus& genus) noexcept;

// HLSL for the table as it is used on the GPU: constant arrays indexed by the
// cloud type code (index 0 = auto-typed storm), so a shader never repeats the
// numbers above. kCloudGenusLight[code] = (ice, anisotropy, baseBrightness,
// elementMeters); kCloudGenusBand[code] = (baseMeters, topMeters,
// extinctionScale, level); kCloudGenusLook[code] = look variant.
[[nodiscard]] std::string CloudGeneraHlsl();
} // namespace orbit::cloud_genera
