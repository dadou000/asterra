// Developer tool: writes an equirectangular PPM of the weather model
// (R = coverage, G = cloud type, B = precipitation) for visual inspection.
#include <orbit/celestial_clouds/WeatherModel.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    using namespace orbit::celestial_clouds;
    const double subsolarDegrees = argc > 1 ? std::atof(argv[1]) : 0.0;
    const double hours = argc > 2 ? std::atof(argv[2]) : 0.0;
    const char* path = argc > 3 ? argv[3] : "weather_map.ppm";
    constexpr int w = 1024;
    constexpr int h = 512;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(w) * h * 3);
    std::vector<unsigned char> cirrus(static_cast<std::size_t>(w) * h);
    double sum = 0.0;
    WeatherParameters parameters;
    parameters.subsolarLatitudeRadians = subsolarDegrees * std::numbers::pi / 180.0;
    parameters.seed = 7;
    for (int y = 0; y < h; ++y)
    {
        const double lat = (0.5 - (y + 0.5) / h) * std::numbers::pi;
        for (int x = 0; x < w; ++x)
        {
            const double lon = ((x + 0.5) / w - 0.5) * 2.0 * std::numbers::pi;
            const orbit::math::Double3 d{
                std::cos(lat) * std::cos(lon), std::sin(lat), std::cos(lat) * std::sin(lon)};
            const auto s = EvaluateWeather(d, hours * 3600.0, parameters, {});
            sum += s.coverage * std::cos(lat);
            auto* p = &pixels[(static_cast<std::size_t>(y) * w + x) * 3];
            p[0] = static_cast<unsigned char>(std::clamp(s.coverage, 0.0, 1.0) * 255.0);
            p[1] = static_cast<unsigned char>(std::clamp(s.cloudType, 0.0, 1.0) * 255.0);
            p[2] = static_cast<unsigned char>(std::clamp(s.precipitation, 0.0, 1.0) * 255.0);
            cirrus[static_cast<std::size_t>(y) * w + x] = static_cast<unsigned char>(std::clamp(s.cirrus, 0.0, 1.0) * 255.0);
        }
    }
    std::FILE* f = std::fopen(path, "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::fwrite(pixels.data(), 1, pixels.size(), f);
    std::fclose(f);
    {
        const std::string cirrusPath = std::string(path) + ".cirrus.pgm";
        std::FILE* c = std::fopen(cirrusPath.c_str(), "wb");
        std::fprintf(c, "P5\n%d %d\n255\n", w, h);
        std::fwrite(cirrus.data(), 1, cirrus.size(), c);
        std::fclose(c);
    }
    // area-weighted mean coverage
    double weight = 0.0;
    for (int y = 0; y < h; ++y)
    {
        weight += std::cos((0.5 - (y + 0.5) / h) * std::numbers::pi) * w;
    }
    std::printf("mean coverage %.3f\n", sum / weight);
    return 0;
}
