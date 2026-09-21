#include <orbit/post_process/LuminanceHistogram.hpp>

#include <array>
#include <cmath>

int main()
{
    using namespace orbit;
    using namespace orbit::post_process;

    std::array<u32, kLuminanceHistogramBins>
        bins{};

    // Normal scene energy is tightly grouped around the middle of the
    // logarithmic range.
    bins[64] = 99'000U;

    const auto baseline =
        AnalyzeLuminanceHistogram(
            bins,
            99'000U);

    if (!baseline.valid ||
        baseline.peakBin != 64U)
    {
        return 1;
    }

    // Add a very small, extremely bright population. Arithmetic linear
    // averaging would move strongly; percentile metering should keep the
    // median anchored while exposing the high percentile/peak separately.
    bins[127] = 1'000U;

    const auto withHighlights =
        AnalyzeLuminanceHistogram(
            bins,
            100'000U);

    if (withHighlights.peakBin != 127U ||
        std::abs(
            withHighlights.medianLog2 -
            baseline.medianLog2) >
            1.0e-6F)
    {
        return 2;
    }

    if (withHighlights.p99Log2 <
            withHighlights.medianLog2 ||
        withHighlights.peakLog2 <=
            withHighlights.medianLog2)
    {
        return 3;
    }

    GpuLuminanceHistogramStatistics gpu{
        .sampleCount = 12U,
        .weightedSampleCount = 2048U,
        .peakBin = 100U,
        .valid = 1U,
        .medianLog2 = -1.0F,
        .p90Log2 = 2.0F,
        .p95Log2 = 3.0F,
        .p99Log2 = 5.0F,
        .medianLuminance = 0.5F,
        .p90Luminance = 4.0F,
        .p95Luminance = 8.0F,
        .p99Luminance = 32.0F,
        .peakLog2 = 8.0F,
        .peakLuminance = 256.0F,
        .meanLog2 = 0.0F,
        .geometricMeanLuminance = 1.0F
    };

    const auto decoded =
        DecodeLuminanceHistogramStatistics(
            gpu);

    if (!decoded.valid ||
        decoded.sampleCount != 12U ||
        decoded.p99Luminance != 32.0F ||
        decoded.peakLuminance != 256.0F)
    {
        return 4;
    }

    return 0;
}
