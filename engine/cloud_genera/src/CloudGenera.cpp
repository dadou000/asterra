#include <orbit/cloud_genera/CloudGenera.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace orbit::cloud_genera
{
namespace
{
using G = CloudGenus;
using L = CloudLevel;

// Heights follow the WMO International Cloud Atlas bands for mid latitudes.
// Extinction scales and phase values are tuned against photographs (see
// docs: /rendering/cloud-genera).
const std::array<CloudGenusInfo, kCloudGenusCount> kTable{{
    {G::Cirrus, "cirrus", L::High, 8000.0, 10000.0, 0.00012, 1.0F, 0.85F, 1.0F, 12000.0, 64000.0, false, 0U, 0U},
    {G::Cirrocumulus, "cirrocumulus", L::High, 7800.0, 8600.0, 0.0005, 1.0F, 0.80F, 1.0F, 500.0, 24000.0, false, 2U, 0U},
    {G::Cirrostratus, "cirrostratus", L::High, 8000.0, 9200.0, 0.0004, 1.0F, 0.85F, 1.0F, 40000.0, 64000.0, false, 0U, 0U},
    {G::Altocumulus, "altocumulus", L::Middle, 4200.0, 5600.0, 0.012, 0.0F, 0.70F, 0.55F, 1100.0, 32000.0, false, 6U, 0U},
    {G::Altostratus, "altostratus", L::Middle, 3100.0, 5300.0, 0.0008, 0.0F, 0.60F, 0.70F, 40000.0, 64000.0, false, 0U, 0U},
    {G::Nimbostratus, "nimbostratus", L::Low, 500.0, 4300.0, 0.0045, 0.0F, 0.45F, 0.28F, 30000.0, 64000.0, false, 4U, 0U},
    {G::Stratocumulus, "stratocumulus", L::Low, 1300.0, 2500.0, 0.007, 0.0F, 0.65F, 0.45F, 3500.0, 32000.0, false, 6U, 0U},
    {G::Stratus, "stratus", L::Low, 150.0, 750.0, 0.004, 0.0F, 0.50F, 0.62F, 20000.0, 64000.0, false, 0U, 0U},
    {G::Cumulus, "cumulus", L::Low, 1100.0, 3500.0, 0.03, 0.0F, 0.72F, 0.45F, 1800.0, 16000.0, false, 7U, 6U},
    {G::Cumulonimbus, "cumulonimbus", L::Vertical, 800.0, 13000.0, 0.8, 0.0F, 0.75F, 0.30F, 15000.0, 64000.0, true, 0U, 0U},
}};
} // namespace

const std::array<CloudGenusInfo, kCloudGenusCount>& CloudGeneraTable() noexcept
{
    return kTable;
}

const CloudGenusInfo& GenusInfo(const CloudGenus genus) noexcept
{
    return kTable[std::min<std::size_t>(static_cast<std::size_t>(genus), kCloudGenusCount - 1U)];
}

std::string_view CloudGenusName(const CloudGenus genus) noexcept
{
    return GenusInfo(genus).name;
}

bool ParseCloudGenus(const std::string_view name, CloudGenus& genus) noexcept
{
    std::string lowered(name);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& info : kTable)
    {
        if (lowered == info.name)
        {
            genus = info.genus;
            return true;
        }
    }
    return false;
}

std::string CloudGeneraHlsl()
{
    std::string light = "static const float4 kCloudGenusLight[11] = {\n    float4(0.0, 0.75, 0.3, 1000.0)";
    std::string look = "static const uint kCloudGenusLook[11] = {\n    0u";
    std::string band = "static const float4 kCloudGenusBand[11] = {\n    float4(0.0, 13000.0, 0.8, 3.0)";
    char buffer[160];
    for (const auto& info : kTable)
    {
        std::snprintf(buffer, sizeof buffer, ",\n    float4(%.3f, %.3f, %.3f, %.1f)", static_cast<double>(info.ice),
            static_cast<double>(info.anisotropy), static_cast<double>(info.baseBrightness), info.elementMeters);
        light += buffer;
        look += ",\n    " + std::to_string(info.lookVariant) + "u";
        std::snprintf(buffer, sizeof buffer, ",\n    float4(%.1f, %.1f, %.5f, %.1f)", info.baseMeters, info.topMeters,
            info.extinctionScale, static_cast<double>(static_cast<std::uint32_t>(info.level)));
        band += buffer;
    }
    // The one extinction model: every renderer turns a genus' density (0-1) into
    // extinction (1/m) here. The boost lifts thin ice and sheets (whose condensate
    // is a few hundredths of a g/kg) without touching an opaque core.
    const char* sigma =
        "float CloudBoost(float d)\n{\n    const float r = max(1.0 - saturate(d), 0.0);\n    return 1.0 - r * r * r;\n}\n"
        "float CloudSigma(uint code, float d)\n{\n    return kCloudGenusBand[code].z * CloudBoost(d);\n}\n";
    return light + "\n};\n" + band + "\n};\n" + look + "\n};\n" + sigma;
}
} // namespace orbit::cloud_genera
