#include <orbit/post_process/HumanEyeAdaptation.hpp>

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
            << "Human-eye adaptation test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}

orbit::post_process::LuminanceHistogramStatistics Stats(
    const float p50,
    const float p95,
    const float p99,
    const float peak)
{
    orbit::post_process::LuminanceHistogramStatistics result{};
    result.valid = true;
    result.medianLog2 = p50;
    result.p95Log2 = p95;
    result.p99Log2 = p99;
    result.peakLog2 = peak;
    result.medianLuminance = std::exp2(p50);
    result.p95Luminance = std::exp2(p95);
    result.p99Luminance = std::exp2(p99);
    result.peakLuminance = std::exp2(peak);
    return result;
}
} // namespace

int main()
{
    using namespace orbit::post_process;

    HumanEyeAdaptationState state{};

    state =
        UpdateHumanEyeAdaptation(
            state,
            Stats(0.0F, 1.0F, 2.0F, 3.0F),
            1.0F / 60.0F);

    Check(state.initialized);
    Check(state.photopicLog2 > 0.0F);
    Check(state.darkAdaptation == 0.0F);
    Check(state.overload == 0.0F);

    const float daylightPhotopic =
        state.photopicLog2;

    for (int frame = 0; frame < 60 * 5; ++frame)
    {
        state =
            UpdateHumanEyeAdaptation(
                state,
                Stats(-10.0F, -9.0F, -8.0F, -7.0F),
                1.0F / 60.0F);
    }

    Check(state.photopicLog2 < daylightPhotopic);
    Check(state.darkAdaptation > 0.20F);

    state =
        UpdateHumanEyeAdaptation(
            state,
            Stats(0.0F, 1.0F, 10.0F, 14.0F),
            1.0F / 60.0F);

    Check(state.overloadTarget > 0.5F);

    for (int frame = 0; frame < 12; ++frame)
    {
        state =
            UpdateHumanEyeAdaptation(
                state,
                Stats(0.0F, 1.0F, 10.0F, 14.0F),
                1.0F / 60.0F);
    }

    Check(state.overload > 0.5F);

    const float overloaded =
        state.overload;

    for (int frame = 0; frame < 30; ++frame)
    {
        state =
            UpdateHumanEyeAdaptation(
                state,
                Stats(0.0F, 1.0F, 2.0F, 3.0F),
                1.0F / 60.0F);
    }

    Check(state.overload < overloaded * 0.25F);
    Check(state.darkAdaptation < 0.25F);

    ResetHumanEyeAdaptation(state);
    Check(!state.initialized);
    Check(state.darkAdaptation == 0.0F);
    Check(state.overload == 0.0F);

    const auto unchanged =
        UpdateHumanEyeAdaptation(
            state,
            {},
            1.0F);

    Check(!unchanged.initialized);

    return 0;
}
