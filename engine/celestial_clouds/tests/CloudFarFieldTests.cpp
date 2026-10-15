#include <orbit/celestial_clouds/CloudFarField.hpp>
#include <orbit/optical_transport/TransmittanceStats.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
} // namespace

int main()
{
    using namespace orbit::celestial_clouds;

    // Synthetic smooth-ish noise volume: lattice values from a hash, tileable, R channel in [0, 1].
    constexpr unsigned grid = 32;
    std::vector<float> noise(static_cast<std::size_t>(grid) * grid * grid * 4U, 0.0F);
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> u(0.0F, 1.0F);
    for (std::size_t v = 0; v < noise.size() / 4U; ++v)
    {
        noise[v * 4U] = 0.5F * u(rng) + 0.5F * u(rng);
    }
    const FarFieldLut lut = BuildFarFieldLut(noise, grid);

    Check(EvaluateFarFieldTransmittance(lut, 0.0F, 2.6F, 20.0F) > 0.999F, "no coverage transmits everything");
    Check(EvaluateFarFieldTransmittance(lut, 0.6F, 2.6F, 0.0F) == 1.0F, "zero scale transmits everything");

    bool monotoneInCoverage = true;
    float previous = 1.0F;
    for (int c = 0; c < kFarFieldCoverageSteps; ++c)
    {
        const float t = EvaluateFarFieldTransmittance(lut, static_cast<float>(c) / (kFarFieldCoverageSteps - 1), 2.6F, 8.0F);
        monotoneInCoverage = monotoneInCoverage && t <= previous + 1.0e-4F;
        previous = t;
    }
    Check(monotoneInCoverage, "more coverage never transmits more");

    bool monotoneInScale = true;
    previous = 1.0F;
    for (float k = 0.05F; k < 80.0F; k *= 1.15F)
    {
        const float t = EvaluateFarFieldTransmittance(lut, 0.55F, 2.0F, k);
        monotoneInScale = monotoneInScale && t <= previous + 1.0e-4F;
        previous = t;
    }
    Check(monotoneInScale, "a thicker step never transmits more");

    // At the stored scales the evaluator returns the stored channel (lattice node, no interpolation error).
    const float node = EvaluateFarFieldTransmittance(lut, 8.0F / 16.0F, 2.0F, 8.0F);
    Check(std::abs(node - lut.channels[2][8][2]) < 1.0e-5F, "evaluator reproduces a stored channel");

    // Off-node value against an independent brute-force estimate on the same noise (different sampling).
    const auto smooth = [](double t) { return t * t * (3.0 - 2.0 * t); };
    const auto sampleR = [&](double px, double py, double pz) {
        const double fx = std::floor(px), fy = std::floor(py), fz = std::floor(pz);
        const double tx = smooth(px - fx), ty = smooth(py - fy), tz = smooth(pz - fz);
        const auto at = [&](long x, long y, long z) {
            const auto w = [](long v) { return static_cast<std::size_t>(((v % 32) + 32) % 32); };
            return static_cast<double>(noise[((w(z) * grid + w(y)) * grid + w(x)) * 4U]);
        };
        const long x = static_cast<long>(fx), y = static_cast<long>(fy), z = static_cast<long>(fz);
        const auto l = [](double a, double b, double t) { return a + (b - a) * t; };
        return l(l(l(at(x, y, z), at(x + 1, y, z), tx), l(at(x, y + 1, z), at(x + 1, y + 1, z), tx), ty),
                 l(l(at(x, y, z + 1), at(x + 1, y, z + 1), tx), l(at(x, y + 1, z + 1), at(x + 1, y + 1, z + 1), tx), ty), tz);
    };
    std::mt19937 rng2(99);
    std::uniform_real_distribution<double> pos(0.0, 32.0);
    const double coverage = 0.43, hardness = 2.2, scale = 5.0;
    double truth = 0.0;
    constexpr int n = 60000;
    for (int i = 0; i < n; ++i)
    {
        const double mix = 0.6 * sampleR(pos(rng2), pos(rng2), pos(rng2)) + 0.4 * sampleR(pos(rng2), pos(rng2), pos(rng2));
        const double b = std::clamp(0.5 + (mix - 0.5) * 1.6, 0.0, 1.0);
        const double s = std::clamp(hardness * (b - (1.0 - coverage)) / std::max(coverage, 0.05), 0.0, 1.0);
        truth += std::exp(-scale * s);
    }
    truth /= n;
    const double tableValue = EvaluateFarFieldTransmittance(lut, static_cast<float>(coverage), static_cast<float>(hardness), static_cast<float>(scale));
    std::printf("off-node transmittance: table %.4f, brute force %.4f\n", tableValue, truth);
    Check(std::abs(tableValue - truth) < 0.04, "table matches an independent estimate off the lattice");

    const std::string block = FarFieldShaderBlock(lut);
    std::size_t entries = 0;
    for (std::size_t at = block.find("float4("); at != std::string::npos; at = block.find("float4(", at + 1))
    {
        ++entries;
    }
    Check(entries == static_cast<std::size_t>(kFarFieldHardnessCount * kFarFieldCoverageSteps), "shader block carries every table entry");
    Check(block.find("float FarFieldTransmittance(") != std::string::npos, "shader block defines the lookup");

    if (failures == 0)
    {
        std::printf("CloudFarField: all tests passed\n");
    }
    return failures;
}
