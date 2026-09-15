#include <orbit/universe/BodyRegistry.hpp>

#include <cassert>
#include <cmath>

int main()
{
    orbit::frames::FrameGraph frames;
    orbit::universe::BodyRegistry bodies(
        frames);

    const auto system =
        bodies.CreateSystem("Helion");

    const auto* systemRecord =
        bodies.FindSystem(system);

    assert(systemRecord != nullptr);
    assert(systemRecord->name == "Helion");

    constexpr orbit::f64 daySeconds =
        86'400.0;
    constexpr orbit::f64 twoPi =
        6.28318530717958647692;

    const auto asterra =
        bodies.CreateBody({
            .system = system,
            .name = "Asterra",
            .shape =
                orbit::universe::SphereShape{
                    .radiusMeters =
                        6'000'000.0
                },
            .mass =
                orbit::universe::MassProperties{
                    .massKilograms =
                        5.0e24
                },
            .transformModel =
                orbit::universe::
                    UniformRotationTransform{
                        .centerInParentMeters = {
                            1.5e11,
                            0.0,
                            0.0
                        },
                        .axisInParent = {
                            0.0,
                            0.0,
                            1.0
                        },
                        .angularVelocityRadiansPerSecond =
                            twoPi /
                            daySeconds
                    }
        });

    const auto luma =
        bodies.CreateBody({
            .system = system,
            .name = "Luma",
            .shape =
                orbit::universe::SphereShape{
                    .radiusMeters =
                        1'500'000.0
                },
            .transformModel =
                orbit::universe::
                    FixedBodyTransform{
                        .parentFromBody = {
                            .translation = {
                                1.5e11 +
                                    400'000'000.0,
                                0.0,
                                0.0
                            }
                        }
                    }
        });

    const auto bodyIds =
        bodies.Bodies(system);

    assert(bodyIds.size() == 2);

    const auto* asterraRecord =
        bodies.FindBody(asterra);
    const auto* lumaRecord =
        bodies.FindBody(luma);

    assert(asterraRecord != nullptr);
    assert(lumaRecord != nullptr);
    assert(
        orbit::universe::
            ReferenceRadiusMeters(
                asterraRecord->shape) ==
        6'000'000.0);

    const auto quarterDay =
        orbit::time::SimulationTime{
            .microsecondsFromEpoch =
                static_cast<orbit::i64>(
                    daySeconds *
                    0.25 *
                    1'000'000.0)
        };

    const auto systemFromBody =
        frames.ResolveTransform(
            asterraRecord->frame,
            systemRecord->inertialFrame,
            quarterDay);

    assert(systemFromBody.has_value());

    // Asterra has completed approximately a quarter turn.
    const auto transformed =
        orbit::math::TransformPoint(
            *systemFromBody,
            {10.0, 0.0, 0.0});

    assert(
        std::abs(
            transformed.x -
            1.5e11) <
        1e-3);
    assert(
        std::abs(
            transformed.y -
            10.0) <
        1e-6);

    return 0;
}
