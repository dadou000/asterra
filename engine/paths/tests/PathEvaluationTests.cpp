#include <orbit/frames/FrameGraph.hpp>
#include <orbit/paths/PathEvaluation.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <cmath>
#include <iostream>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr << "Path evaluation test failed: " #expression \
                      << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace
{
[[nodiscard]] bool Near(
    const orbit::f64 left,
    const orbit::f64 right,
    const orbit::f64 epsilon = 1.0e-8)
{
    return std::abs(left - right) <= epsilon;
}
}

int main()
{
    const orbit::math::Double3 start{
        0.0, 0.0, 0.0};
    const orbit::math::Double3 end{
        10.0, 0.0, 0.0};

    ORBIT_TEST_CHECK(
        orbit::paths::EvaluateDirect(
            start,
            end,
            0.25) ==
        orbit::math::Double3{
            2.5, 0.0, 0.0});

    const orbit::math::Double3 startHandle{
        0.0, 4.0, 0.0};
    const orbit::math::Double3 endHandle{
        0.0, 4.0, 0.0};

    ORBIT_TEST_CHECK(
        orbit::paths::EvaluateBezier(
            start,
            end,
            startHandle,
            endHandle,
            0.0) == start);
    ORBIT_TEST_CHECK(
        orbit::paths::EvaluateBezier(
            start,
            end,
            startHandle,
            endHandle,
            1.0) == end);
    ORBIT_TEST_CHECK(
        orbit::paths::EvaluateBezierCurvature(
            start,
            end,
            startHandle,
            endHandle,
            0.5) > 0.0);

    orbit::frames::FrameGraph frames;
    orbit::universe::BodyRegistry bodies(frames);
    const auto system =
        bodies.CreateSystem("Test System");
    const auto* systemRecord =
        bodies.FindSystem(system);
    ORBIT_TEST_CHECK(systemRecord != nullptr);

    const auto body =
        bodies.CreateBody({
            .system = system,
            .name = "Asterra",
            .shape =
                orbit::universe::SphereShape{
                    .radiusMeters = 1000.0
                },
            .transformModel =
                orbit::universe::UniformRotationTransform{
                    .centerInParentMeters = {
                        5000.0, 0.0, 0.0
                    },
                    .axisInParent = {
                        0.0, 1.0, 0.0
                    },
                    .angularVelocityRadiansPerSecond =
                        0.1
                }
        });

    const orbit::paths::SurfaceAnchor surface{
        .body = body,
        .coordinate = {
            0.0,
            0.0,
            5.0
        }
    };

    const auto surfaceAtZero =
        orbit::paths::ResolveAnchor(
            surface,
            systemRecord->inertialFrame,
            {.microsecondsFromEpoch = 0},
            frames,
            bodies);

    ORBIT_TEST_CHECK(surfaceAtZero.has_value());
    ORBIT_TEST_CHECK(
        Near(
            surfaceAtZero->localMeters.x,
            6005.0));

    const auto bodyRecord =
        bodies.FindBody(body);
    ORBIT_TEST_CHECK(bodyRecord != nullptr);

    const auto entityId =
        orbit::scene::ObjectId::Random();

    const orbit::paths::EntitySocketAnchor entity{
        .entity = entityId,
        .socket = "deck.out",
        .localMeters = {
            2.0, 0.0, 0.0
        }
    };

    const auto entityResolved =
        orbit::paths::ResolveAnchor(
            entity,
            systemRecord->inertialFrame,
            {.microsecondsFromEpoch = 0},
            frames,
            bodies,
            [entityId,
             frame = bodyRecord->frame](
                const orbit::scene::ObjectId object,
                const std::string_view socket,
                const orbit::math::Double3 local)
                -> std::optional<orbit::frames::FramePoint>
            {
                if (object != entityId ||
                    socket != "deck.out")
                {
                    return std::nullopt;
                }

                return orbit::frames::FramePoint{
                    .frame = frame,
                    .localMeters = local
                };
            });

    ORBIT_TEST_CHECK(entityResolved.has_value());
    ORBIT_TEST_CHECK(
        Near(entityResolved->localMeters.x, 5002.0));

    return 0;
}
