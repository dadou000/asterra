#include <orbit/frames/FrameGraph.hpp>

#include <cassert>
#include <cmath>

namespace
{
[[nodiscard]] bool Near(
    const orbit::f64 a,
    const orbit::f64 b,
    const orbit::f64 epsilon = 1e-8)
{
    return std::abs(a - b) <= epsilon;
}

[[nodiscard]] orbit::math::Double3x3
RotationZ(const orbit::f64 radians)
{
    const orbit::f64 c = std::cos(radians);
    const orbit::f64 s = std::sin(radians);

    return {
        .xAxis = {c, s, 0.0},
        .yAxis = {-s, c, 0.0},
        .zAxis = {0.0, 0.0, 1.0}
    };
}
}

int main()
{
    orbit::frames::FrameGraph graph;

    const auto systemFrame =
        graph.CreateRoot();

    const auto planetFrame =
        graph.CreateFrame(
            systemFrame,
            [](const orbit::time::SimulationTime time)
            {
                const orbit::f64 seconds =
                    static_cast<orbit::f64>(
                        time.microsecondsFromEpoch) /
                    1'000'000.0;

                return orbit::math::RigidTransformD{
                    .rotation =
                        RotationZ(seconds),
                    .translation = {
                        1.5e11,
                        -2.0e10,
                        3.0e9
                    }
                };
            });

    const auto vehicleFrame =
        graph.CreateFrame(
            planetFrame,
            [](const orbit::time::SimulationTime time)
            {
                const orbit::f64 seconds =
                    static_cast<orbit::f64>(
                        time.microsecondsFromEpoch) /
                    1'000'000.0;

                return orbit::math::RigidTransformD{
                    .translation = {
                        100.0 + seconds * 5.0,
                        20.0,
                        3.0
                    }
                };
            });

    // A nearby point and camera share the planet frame. Their 1 cm
    // separation must not be lost merely because the planet itself is
    // 150 million km from the system root.
    const orbit::frames::FramePoint camera{
        .frame = planetFrame,
        .localMeters = {
            6'000'100.0,
            0.0,
            0.0
        }
    };

    const orbit::frames::FramePoint nearby{
        .frame = planetFrame,
        .localMeters = {
            6'000'100.01,
            0.0,
            0.0
        }
    };

    const auto relative =
        graph.ToCameraRelative(
            nearby,
            camera,
            {.microsecondsFromEpoch = 0});

    assert(relative.has_value());
    assert(
        std::abs(
            relative->x - 0.01F) <
        1e-5F);

    // Rotating body: after pi/2 seconds the body's +X axis points along
    // system +Y.
    constexpr orbit::f64 halfPi =
        1.57079632679489661923;

    const auto rotated =
        graph.TransformPoint(
            {
                .frame = planetFrame,
                .localMeters = {
                    10.0,
                    0.0,
                    0.0
                }
            },
            systemFrame,
            {
                .microsecondsFromEpoch =
                    static_cast<orbit::i64>(
                        halfPi *
                        1'000'000.0)
            });

    assert(rotated.has_value());
    assert(
        Near(
            rotated->localMeters.x,
            1.5e11,
            2e-5));
    assert(
        Near(
            rotated->localMeters.y,
            -2.0e10 + 10.0,
            2e-5));

    // Moving vehicle: the point remains vehicle-local while resolving
    // to a changing planet-frame position as simulation time advances.
    const orbit::frames::FramePoint
        vehiclePoint{
            .frame = vehicleFrame,
            .localMeters = {
                2.0,
                0.0,
                1.0
            }
        };

    const auto atZero =
        graph.TransformPoint(
            vehiclePoint,
            planetFrame,
            {.microsecondsFromEpoch = 0});

    const auto atTwoSeconds =
        graph.TransformPoint(
            vehiclePoint,
            planetFrame,
            {.microsecondsFromEpoch = 2'000'000});

    assert(atZero.has_value());
    assert(atTwoSeconds.has_value());

    assert(
        Near(
            atZero->localMeters.x,
            102.0));
    assert(
        Near(
            atTwoSeconds->localMeters.x,
            112.0));

    // Disconnected roots intentionally cannot be converted.
    const auto anotherRoot =
        graph.CreateRoot();

    assert(
        !graph.ResolveTransform(
            vehicleFrame,
            anotherRoot,
            {}).has_value());

    return 0;
}
