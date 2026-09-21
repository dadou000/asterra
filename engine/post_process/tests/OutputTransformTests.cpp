#include <orbit/post_process/OutputTransform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <source_location>

namespace
{
void Check(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "Output-transform test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}
} // namespace

int main()
{
    using namespace orbit::post_process;

    OutputTransformSettings autoMode{};
    OutputDisplayCapabilities noHdr{};

    const auto autoSdr =
        ResolveOutputTransform(
            autoMode,
            noHdr);

    Check(autoSdr.resolvedMode ==
        OutputMode::Sdr);
    Check(!autoSdr.fellBackToSdr);

    OutputTransformSettings forcedHdr{};
    forcedHdr.mode =
        OutputMode::Hdr10;

    const auto fallback =
        ResolveOutputTransform(
            forcedHdr,
            noHdr);

    Check(fallback.resolvedMode ==
        OutputMode::Sdr);
    Check(fallback.fellBackToSdr);

    OutputDisplayCapabilities hdr{
        .hdr10Supported = true,
        .reportedPeakNits = 600.0F
    };

    const auto hdrResolved =
        ResolveOutputTransform(
            forcedHdr,
            hdr);

    Check(hdrResolved.resolvedMode ==
        OutputMode::Hdr10);
    Check(std::abs(
              hdrResolved.resolvedPeakNits -
              600.0F) <
          1.0e-6F);

    Check(std::abs(
              EncodeSrgbChannel(0.0F)) <
          1.0e-6F);
    Check(std::abs(
              EncodeSrgbChannel(1.0F) -
              1.0F) <
          1.0e-5F);

    for (const float nits :
         {0.1F, 1.0F, 100.0F, 203.0F,
          600.0F, 1000.0F, 4000.0F})
    {
        const float encoded =
            EncodeSt2084FromNits(
                nits);
        const float decoded =
            DecodeSt2084ToNits(
                encoded);

        const float tolerance =
            std::max(
                1.0e-3F,
                nits * 2.0e-4F);

        Check(std::abs(
                  decoded - nits) <=
              tolerance);
    }

    return 0;
}
