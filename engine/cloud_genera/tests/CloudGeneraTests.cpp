#include <orbit/cloud_genera/CloudGenera.hpp>

#include <cstdio>
#include <cstdlib>

using namespace orbit::cloud_genera;

namespace
{
int failures = 0;

void Check(const bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
} // namespace

int main()
{
    const auto& table = CloudGeneraTable();
    Check(table.size() == 10U, "ten genera");
    for (std::size_t i = 0; i < table.size(); ++i)
    {
        const auto& info = table[i];
        Check(static_cast<std::size_t>(info.genus) == i, "table is in enum order");
        Check(info.topMeters > info.baseMeters, "top above base");
        Check(info.extinctionScale > 0.0, "positive extinction");
        Check(info.baseBrightness > 0.0F && info.baseBrightness <= 1.0F, "base brightness in range");
        CloudGenus parsed{};
        Check(ParseCloudGenus(info.name, parsed) && parsed == info.genus, "name round trips");
        Check(CloudTypeCode(info.genus) == i + 1U, "type codes start at 1");
    }
    // WMO height bands: high ice above 6 km, low cloud base under 2 km.
    Check(GenusInfo(CloudGenus::Cirrus).baseMeters >= 6000.0, "cirrus is high");
    Check(GenusInfo(CloudGenus::Altocumulus).baseMeters > 2000.0 &&
              GenusInfo(CloudGenus::Altocumulus).topMeters < 7000.0,
        "altocumulus is mid level");
    Check(GenusInfo(CloudGenus::Stratus).baseMeters < 600.0, "stratus is lowest");
    Check(GenusInfo(CloudGenus::Cirrus).ice > 0.5F && GenusInfo(CloudGenus::Cumulus).ice < 0.5F,
        "only high clouds are ice");
    CloudGenus unknown{};
    Check(!ParseCloudGenus("nope", unknown), "unknown name rejected");
    Check(CloudGeneraHlsl().find("kCloudGenusLight[11]") != std::string::npos, "hlsl table emitted");
    Check(CloudGeneraHlsl().find("float CloudSigma(uint code, float d)") != std::string::npos, "one extinction model emitted");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
