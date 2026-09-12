#include <orbit/world/WorldPosition.hpp>

#include <cmath>
#include <iostream>

namespace
{
bool NearlyEqual(const float a, const float b, const float epsilon)
{
    return std::fabs(a - b) <= epsilon;
}
} // namespace

int main()
{
    const orbit::world::WorldPosition camera{
        .meters = {
            4'000'000.0,
            -3'000'000.0,
            2'000'000.0
        }
    };

    const orbit::world::WorldPosition object{
        .meters = {
            4'000'000.125,
            -2'999'998.5,
            1'999'980.0
        }
    };

    const orbit::math::Float3 relative =
        orbit::world::ToCameraRelative(object, camera);

    if (!NearlyEqual(relative.x, 0.125F, 0.00001F) ||
        !NearlyEqual(relative.y, 1.5F, 0.00001F) ||
        !NearlyEqual(relative.z, -20.0F, 0.00001F))
    {
        std::cerr << "Camera-relative conversion failed.\n";
        return 1;
    }

    const orbit::f64 expectedDistanceSquared =
        0.125 * 0.125 +
        1.5 * 1.5 +
        20.0 * 20.0;

    const orbit::f64 actualDistanceSquared =
        orbit::world::DistanceSquared(object, camera);

    if (std::fabs(actualDistanceSquared - expectedDistanceSquared) > 1.0e-9)
    {
        std::cerr << "World-space distance calculation failed.\n";
        return 1;
    }

    return 0;
}
