#include <orbit/weather_lab/CloudLayers.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
using namespace orbit::weather_lab;

int Fail(const char* what)
{
    std::fprintf(stderr, "CloudLayersTests: %s\n", what);
    return 1;
}

CloudLayerSettings Small(const CloudLayerKind kind)
{
    CloudLayerSettings settings;
    settings.kind = kind;
    settings.resolutionX = 64U;
    settings.resolutionY = 64U;
    settings.resolutionZ = 16U;
    settings.coverage = 0.6F;
    return settings;
}

std::vector<CloudLayerKind> GeneratedKinds()
{
    std::vector<CloudLayerKind> kinds;
    for (const auto& info : orbit::cloud_genera::CloudGeneraTable())
    {
        if (IsGeneratedCloudLayer(info.genus)) kinds.push_back(info.genus);
    }
    return kinds;
}

double Mean(const std::vector<float>& values)
{
    double sum = 0.0;
    for (const float v : values) sum += v;
    return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
}
} // namespace

int main()
{
    // Names round-trip and the codes are the Volume Cloud Type values (0 is the storm).
    for (const auto kind : GeneratedKinds())
    {
        CloudLayerKind parsed{};
        if (!ParseCloudLayerKind(CloudLayerKindName(kind), parsed) || parsed != kind) return Fail("name round trip");
        if (CloudTypeCode(kind) == 0U) return Fail("type code collides with the storm");
        if (DefaultCloudLayerPlacement(kind).halfThicknessMeters <= 0.0) return Fail("placement");
    }
    CloudLayerKind unknown{};
    if (ParseCloudLayerKind("nimbus", unknown)) return Fail("unknown kind accepted");
    if (GeneratedKinds().size() != 9U) return Fail("nine genera are generated, cumulonimbus is simulated");
    if (!GenerateCloudLayerDensity(Small(CloudLayerKind::Cumulonimbus)).empty()) return Fail("cumulonimbus is simulated");

    for (const auto kind : GeneratedKinds())
    {
        const auto settings = Small(kind);
        const auto a = GenerateCloudLayerDensity(settings);
        const auto b = GenerateCloudLayerDensity(settings);
        if (a.size() != 64U * 64U * 16U) return Fail("size");
        if (a != b) return Fail("not deterministic");
        for (const float v : a)
        {
            if (!(v >= 0.0F && v <= 1.0F)) return Fail("density outside [0, 1]");
        }
        if (Mean(a) <= 0.0) return Fail("layer is empty");
        auto other = settings;
        other.seed = 99U;
        if (GenerateCloudLayerDensity(other) == a) return Fail("seed has no effect");
        auto clear = settings;
        clear.coverage = 0.0F;
        auto overcast = settings;
        overcast.coverage = 1.0F;
        if (Mean(GenerateCloudLayerDensity(overcast)) < Mean(GenerateCloudLayerDensity(clear)))
        {
            return Fail("coverage does not increase cloud");
        }
    }

    // Cirrus is drawn out into fibres along the wind: correlation along the
    // wind is much longer than across it.
    {
        CloudLayerSettings settings = Small(CloudLayerKind::Cirrus);
        settings.resolutionX = 128U;
        settings.resolutionY = 128U;
        settings.resolutionZ = 8U;
        // Fibres are hundreds of metres across: resolve them.
        settings.halfExtentX = 16000.0;
        settings.halfExtentY = 16000.0;
        settings.coverage = 1.0F;
        settings.windX = 1.0F;
        settings.windY = 0.0F;
        const auto d = GenerateCloudLayerDensity(settings);
        const auto at = [&](const unsigned i, const unsigned j) { return d[(4U * 128U + j) * 128U + i]; };
        double alongDiff = 0.0;
        double acrossDiff = 0.0;
        for (unsigned j = 0U; j < 120U; ++j)
        {
            for (unsigned i = 0U; i < 120U; ++i)
            {
                alongDiff += std::abs(at(i + 4U, j) - at(i, j));
                acrossDiff += std::abs(at(i, j + 4U) - at(i, j));
            }
        }
        if (!(acrossDiff > 1.5 * alongDiff)) return Fail("cirrus is not stretched along the wind");
    }
    return 0;
}
