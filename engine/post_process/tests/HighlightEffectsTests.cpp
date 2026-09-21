#include <orbit/post_process/HighlightEffects.hpp>

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
            << "Highlight-effects test failed at "
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

    const HighlightEffectsConfig config{};

    const auto dim =
        ClassifyHighlight(
            0.5F,
            0.5F,
            config);

    Check(dim.bloom == 0.0F);
    Check(dim.glare == 0.0F);
    Check(dim.flare == 0.0F);

    const auto bloom =
        ClassifyHighlight(
            1.25F,
            1.0F,
            config);

    Check(bloom.bloom > 0.0F);
    Check(bloom.glare == 0.0F);
    Check(bloom.flare == 0.0F);

    const auto glare =
        ClassifyHighlight(
            4.0F,
            3.5F,
            config);

    Check(glare.bloom > 0.0F);
    Check(glare.glare > 0.0F);
    Check(glare.flare == 0.0F);

    const auto broadBright =
        ClassifyHighlight(
            8.0F,
            7.0F,
            config);

    Check(broadBright.bloom > 0.0F);
    Check(broadBright.glare > 0.0F);
    Check(broadBright.flare == 0.0F);

    const auto compactExtreme =
        ClassifyHighlight(
            8.0F,
            1.0F,
            config);

    Check(compactExtreme.flare > 0.0F);

    HighlightEffectsConfig noBloom = config;
    noBloom.bloomEnabled = false;

    const auto disabled =
        ClassifyHighlight(
            8.0F,
            1.0F,
            noBloom);

    Check(disabled.bloom == 0.0F);
    Check(disabled.glare > 0.0F);
    Check(disabled.flare > 0.0F);

    return 0;
}
