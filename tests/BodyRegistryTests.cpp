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

    if (asterraRecord == nullptr ||
        lumaRecord == nullptr)
    {
        return 1;
    }
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

    const orbit::universe::SystemId
        persistentSystem{
            .high = 0x1010101010101010ULL,
            .low = 0x2020202020202020ULL
        };
    const orbit::frames::FrameId
        persistentSystemFrame{
            .high = 0x3030303030303030ULL,
            .low = 0x4040404040404040ULL
        };

    const auto stableSystem =
        bodies.CreateSystem(
            "Persistent System",
            persistentSystem,
            persistentSystemFrame);

    assert(stableSystem == persistentSystem);
    assert(
        bodies.FindSystem(stableSystem)->
            inertialFrame ==
        persistentSystemFrame);

    const orbit::universe::BodyId
        persistentBody{
            .high = 0x5050505050505050ULL,
            .low = 0x6060606060606060ULL
        };
    const orbit::frames::FrameId
        persistentBodyFrame{
            .high = 0x7070707070707070ULL,
            .low = 0x8080808080808080ULL
        };

    const auto stableBody =
        bodies.CreateBody({
            .system = stableSystem,
            .name = "Persistent Body",
            .shape =
                orbit::universe::SphereShape{
                    .radiusMeters = 42.0
                },
            .id = persistentBody,
            .frame = persistentBodyFrame
        });

    assert(stableBody == persistentBody);
    assert(
        bodies.FindBody(stableBody)->
            frame ==
        persistentBodyFrame);
    assert(
        frames.Contains(
            persistentBodyFrame));
    assert(
        frames.Parent(
            persistentBodyFrame) ==
        std::optional<orbit::frames::FrameId>(
            persistentSystemFrame));

    return 0;
}
