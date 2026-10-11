#include "orbit/celestial_clouds/CloudFarField.hpp"

#include <orbit/optical_transport/TransmittanceStats.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace orbit::celestial_clouds
{

namespace
{
double Halton(unsigned index, unsigned base)
{
    double f = 1.0, r = 0.0;
    for (unsigned i = index; i > 0; i /= base)
    {
        f /= static_cast<double>(base);
        r += f * static_cast<double>(i % base);
    }
    return r;
}

double SmoothStep(double t) { return t * t * (3.0 - 2.0 * t); }

// Trilinear lookup with smoothstep weights of the R channel, p in voxel units (tileable), as SampleNoise does.
double SampleR(std::span<const float> noise, unsigned grid, double px, double py, double pz)
{
    const double fx = std::floor(px), fy = std::floor(py), fz = std::floor(pz);
    const double tx = SmoothStep(px - fx), ty = SmoothStep(py - fy), tz = SmoothStep(pz - fz);
    const long x = static_cast<long>(fx), y = static_cast<long>(fy), z = static_cast<long>(fz);
    const long n = static_cast<long>(grid);
    const auto at = [&](long ix, long iy, long iz) {
        const auto w = [n](long v) { return static_cast<std::size_t>(((v % n) + n) % n); };
        return static_cast<double>(noise[((w(iz) * grid + w(iy)) * grid + w(ix)) * 4U]);
    };
    const auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    return lerp(lerp(lerp(at(x, y, z), at(x + 1, y, z), tx), lerp(at(x, y + 1, z), at(x + 1, y + 1, z), tx), ty),
                lerp(lerp(at(x, y, z + 1), at(x + 1, y, z + 1), tx), lerp(at(x, y + 1, z + 1), at(x + 1, y + 1, z + 1), tx), ty), tz);
}
} // namespace

FarFieldLut BuildFarFieldLut(std::span<const float> noiseRgba, unsigned grid)
{
    // Distribution of the shader's baseValue (no billow octave, which is filtered out at these footprints):
    //   baseMix = 0.6 R(a) + 0.4 R(b) at two independent positions, baseValue = saturate(0.5 + (baseMix - 0.5) 1.6).
    constexpr unsigned kSamples = 8192;
    std::vector<double> base(kSamples);
    const double g = static_cast<double>(grid);
    for (unsigned i = 0; i < kSamples; ++i)
    {
        const double a = SampleR(noiseRgba, grid, Halton(i + 1, 2) * g, Halton(i + 1, 3) * g, Halton(i + 1, 5) * g);
        const double b = SampleR(noiseRgba, grid, Halton(i + 1, 7) * g, Halton(i + 1, 11) * g, Halton(i + 1, 13) * g);
        const double mix = 0.6 * a + 0.4 * b;
        base[i] = std::clamp(0.5 + (mix - 0.5) * 1.6, 0.0, 1.0);
    }

    FarFieldLut lut;
    std::vector<double> depths(kSamples);
    for (int h = 0; h < kFarFieldHardnessCount; ++h)
    {
        const double hardness = kFarFieldHardnessMin + (kFarFieldHardnessMax - kFarFieldHardnessMin) *
                                                           static_cast<double>(h) / (kFarFieldHardnessCount - 1);
        for (int c = 0; c < kFarFieldCoverageSteps; ++c)
        {
            const double coverage = static_cast<double>(c) / (kFarFieldCoverageSteps - 1);
            for (unsigned i = 0; i < kSamples; ++i)
            {
                // The shader's: saturate(w (baseValue - (1 - coverage)) / max(coverage, 0.05)).
                depths[i] = std::clamp(hardness * (base[i] - (1.0 - coverage)) / std::max(coverage, 0.05), 0.0, 1.0);
            }
            const auto stats = optical_transport::FromSamples(depths);
            for (int j = 0; j < 4; ++j)
            {
                lut.channels[static_cast<std::size_t>(h)][static_cast<std::size_t>(c)][static_cast<std::size_t>(j)] =
                    static_cast<float>(stats.channel[static_cast<std::size_t>(j)]);
            }
        }
    }
    return lut;
}

float EvaluateFarFieldTransmittance(const FarFieldLut& lut, float coverage, float hardness, float scale)
{
    if (scale <= 0.0F)
    {
        return 1.0F;
    }
    const float cu = std::clamp(coverage, 0.0F, 1.0F) * (kFarFieldCoverageSteps - 1);
    const float hu = std::clamp((hardness - kFarFieldHardnessMin) / (kFarFieldHardnessMax - kFarFieldHardnessMin), 0.0F, 1.0F) *
                     (kFarFieldHardnessCount - 1);
    const int c0 = std::min(static_cast<int>(cu), kFarFieldCoverageSteps - 2);
    const int h0 = std::min(static_cast<int>(hu), kFarFieldHardnessCount - 2);
    const float cf = cu - static_cast<float>(c0), hf = hu - static_cast<float>(h0);
    std::array<float, 4> l{};
    for (std::size_t j = 0; j < 4; ++j)
    {
        const auto at = [&](int hh, int cc) { return lut.channels[static_cast<std::size_t>(hh)][static_cast<std::size_t>(cc)][j]; };
        const float lo = at(h0, c0) + (at(h0, c0 + 1) - at(h0, c0)) * cf;
        const float hi = at(h0 + 1, c0) + (at(h0 + 1, c0 + 1) - at(h0 + 1, c0)) * cf;
        l[j] = lo + (hi - lo) * hf;
    }
    constexpr float k[4] = {0.5F, 2.0F, 8.0F, 32.0F};
    if (scale >= k[3])
    {
        return l[3];
    }
    const auto depth = [&](int j) { return -std::log(std::max(l[static_cast<std::size_t>(j)], 1.0e-6F)); };
    if (scale <= k[0])
    {
        return std::exp(-depth(0) * scale / k[0]);
    }
    const int a = scale < k[1] ? 0 : (scale < k[2] ? 1 : 2);
    const float ya = depth(a), yb = depth(a + 1);
    const float u = (std::log(scale) - std::log(k[a])) / (std::log(k[a + 1]) - std::log(k[a]));
    if (ya < 1.0e-6F || yb < 1.0e-6F)
    {
        return std::exp(-(ya + (yb - ya) * u));
    }
    return std::exp(-std::exp(std::log(ya) + (std::log(yb) - std::log(ya)) * u));
}

std::string FarFieldShaderBlock(const FarFieldLut& lut)
{
    std::string text;
    char number[64];
    text += "\n// ---- footprint-filtered far field (generated from the baked cloud noise; see CloudFarField.hpp) ----\n";
    std::snprintf(number, sizeof number, "static const uint kFarHardnessCount = %d;\n", kFarFieldHardnessCount);
    text += number;
    std::snprintf(number, sizeof number, "static const uint kFarCoverageSteps = %d;\n", kFarFieldCoverageSteps);
    text += number;
    text += "static const float4 kFarLut[" + std::to_string(kFarFieldHardnessCount * kFarFieldCoverageSteps) + "] = {\n";
    for (int h = 0; h < kFarFieldHardnessCount; ++h)
    {
        for (int c = 0; c < kFarFieldCoverageSteps; ++c)
        {
            const auto& v = lut.channels[static_cast<std::size_t>(h)][static_cast<std::size_t>(c)];
            std::snprintf(number, sizeof number, "    float4(%.7f, %.7f, %.7f, %.7f)%s\n", v[0], v[1], v[2], v[3],
                          (h == kFarFieldHardnessCount - 1 && c == kFarFieldCoverageSteps - 1) ? "" : ",");
            text += number;
        }
    }
    text += "};\n";
    text += R"(
// Transmittance mean(exp(-K s)) of a footprint at coverage and type hardness; mirrors EvaluateFarFieldTransmittance.
float FarFieldTransmittance(float coverage, float hardness, float K)
{
    if (K <= 0.0)
    {
        return 1.0;
    }
    const float cu = saturate(coverage) * float(kFarCoverageSteps - 1u);
    const float hu = saturate((hardness - 1.0) / 2.0) * float(kFarHardnessCount - 1u);
    const uint c0 = min((uint)cu, kFarCoverageSteps - 2u);
    const uint h0 = min((uint)hu, kFarHardnessCount - 2u);
    const float cf = cu - float(c0);
    const float hf = hu - float(h0);
    const float4 lo = lerp(kFarLut[h0 * kFarCoverageSteps + c0], kFarLut[h0 * kFarCoverageSteps + c0 + 1u], cf);
    const float4 hi = lerp(kFarLut[(h0 + 1u) * kFarCoverageSteps + c0], kFarLut[(h0 + 1u) * kFarCoverageSteps + c0 + 1u], cf);
    const float4 l = lerp(lo, hi, hf);
    if (K >= 32.0)
    {
        return l.w;
    }
    const float4 y = -log(max(l, 1.0e-6));
    if (K <= 0.5)
    {
        return exp(-y.x * K / 0.5);
    }
    float ya;
    float yb;
    float u;
    if (K < 2.0)
    {
        ya = y.x; yb = y.y; u = log(K / 0.5) / log(4.0);
    }
    else if (K < 8.0)
    {
        ya = y.y; yb = y.z; u = log(K / 2.0) / log(4.0);
    }
    else
    {
        ya = y.z; yb = y.w; u = log(K / 8.0) / log(4.0);
    }
    if (ya < 1.0e-6 || yb < 1.0e-6)
    {
        return exp(-(ya + (yb - ya) * u));
    }
    return exp(-exp(log(ya) + (log(yb) - log(ya)) * u));
}
)";
    return text;
}

} // namespace orbit::celestial_clouds
