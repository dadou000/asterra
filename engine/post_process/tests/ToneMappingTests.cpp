#include <orbit/post_process/ToneMapping.hpp>

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
            << "Tone-mapping test failed at "
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

    ToneMappingConfig config{};
    const float headroom =
        DisplayHeadroomRatio(config);

    Check(headroom > 4.9F);
    Check(headroom < 5.0F);

    // Default curve is linear through reference white.
    Check(std::abs(
              ToneMapLuminance(0.18F, config) -
              0.18F) <
          1.0e-6F);
    Check(std::abs(
              ToneMapLuminance(1.0F, config) -
              1.0F) <
          1.0e-6F);

    float previous = 0.0F;
    for (int i = 0; i <= 10000; ++i)
    {
        const float x =
            static_cast<float>(i) * 0.01F;
        const float y =
            ToneMapLuminance(x, config);

        Check(y >= previous - 1.0e-6F);
        Check(y <= headroom + 1.0e-5F);
        previous = y;
    }

    // Very bright values retain separation while approaching display peak.
    const float bright =
        ToneMapLuminance(10.0F, config);
    const float brighter =
        ToneMapLuminance(20.0F, config);
    const float extreme =
        ToneMapLuminance(1000.0F, config);

    Check(bright > 1.0F);
    Check(brighter > bright);
    Check(extreme > brighter);
    Check(extreme <= headroom);

    ToneMappingConfig sdrLike{};
    sdrLike.referenceWhiteNits = 100.0F;
    sdrLike.peakNits = 100.0F;

    Check(std::abs(
              DisplayHeadroomRatio(sdrLike) -
              1.0F) <
          1.0e-6F);
    Check(std::abs(
              ToneMapLuminance(100.0F, sdrLike) -
              1.0F) <
          1.0e-6F);

    ToneMappingConfig disabled = config;
    disabled.enabled = false;
    Check(std::abs(
              ToneMapLuminance(12.0F, disabled) -
              12.0F) <
          1.0e-6F);

    const auto diagnostics =
        EvaluateToneMappingDiagnostics(config);

    Check(std::abs(
              diagnostics.headroomRatio -
              headroom) <
          1.0e-6F);
    Check(std::abs(
              diagnostics.mappedReferenceWhite -
              1.0F) <
          1.0e-6F);

    return 0;
}
