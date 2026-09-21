#include <orbit/celestial_gravity/GravityService.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

namespace
{
bool Near(double a, double b, double rel = 1.0e-10)
{
    const double scale =
        std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= rel * scale;
}
} // namespace

int main()
{
    orbit::frames::FrameGraph frames;

    const auto root =
        frames.CreateRoot();

    const auto sourceFrame =
        frames.CreateFrame(
            root,
            [](orbit::time::SimulationTime)
            {
                return orbit::math::RigidTransformD{
                    .translation = {10.0, 0.0, 0.0}
                };
            });

    orbit::celestial_gravity::GravityService gravity(
        frames);

    const orbit::celestial_gravity::GravitySourceId sourceId{
        .high = 1,
        .low = 2
    };

    gravity.RegisterSource({
        .id = sourceId,
        .frame = sourceFrame,
        .model = std::make_shared<
            orbit::celestial_gravity::PointMassGravityModel>(
                100.0)
    });

    const auto a =
        gravity.AccelerationFrom(
            sourceId,
            orbit::frames::FramePoint{
                .frame = root,
                .localMeters = {20.0, 0.0, 0.0}
            },
            orbit::time::SimulationTime{});

    if (!a.has_value() ||
        !Near(a->x, -1.0) ||
        !Near(a->y, 0.0) ||
        !Near(a->z, 0.0))
    {
        return 1;
    }

    const auto total =
        gravity.TotalAcceleration(
            orbit::frames::FramePoint{
                .frame = root,
                .localMeters = {20.0, 0.0, 0.0}
            },
            orbit::time::SimulationTime{});

    if (!total.has_value() ||
        !Near(total->x, -1.0))
    {
        return 2;
    }

    if (!Near(
            orbit::celestial_gravity::
                GravitationalParameterFromMass(
                    5.9722e24),
            3.986025446e14,
            1.0e-9))
    {
        return 3;
    }

    return 0;
}
