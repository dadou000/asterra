#include <orbit/render_view/RenderView.hpp>

#include <cmath>
#include <iostream>

#define ORBIT_TEST_CHECK(expression) \
    do \
    { \
        if (!(expression)) \
        { \
            std::cerr << "ViewRay test failed: " #expression \
                      << " at line " << __LINE__ << '\n'; \
            return 1; \
        } \
    } while (false)

namespace
{
[[nodiscard]] bool Near(
    const orbit::f64 left,
    const orbit::f64 right,
    const orbit::f64 epsilon = 1.0e-6)
{
    return std::abs(left - right) <= epsilon;
}
}

int main()
{
    orbit::render_view::CameraState camera{
        .localPositionMeters = {
            0.0,
            0.0,
            -10.0
        },
        .forward = {
            0.0F,
            0.0F,
            1.0F
        },
        .up = {
            0.0F,
            1.0F,
            0.0F
        },
        .verticalFovRadians =
            1.57079632679F
    };

    const auto center =
        orbit::render_view::ViewportRay(
            camera,
            100,
            100,
            0.5F,
            0.5F);

    ORBIT_TEST_CHECK(center.has_value());
    ORBIT_TEST_CHECK(
        center->origin ==
        camera.localPositionMeters);
    ORBIT_TEST_CHECK(
        Near(center->direction.x, 0.0));
    ORBIT_TEST_CHECK(
        Near(center->direction.y, 0.0));
    ORBIT_TEST_CHECK(
        Near(center->direction.z, 1.0));

    const auto topLeft =
        orbit::render_view::ViewportRay(
            camera,
            200,
            100,
            0.0F,
            0.0F);

    ORBIT_TEST_CHECK(topLeft.has_value());
    ORBIT_TEST_CHECK(
        topLeft->direction.x < 0.0);
    ORBIT_TEST_CHECK(
        topLeft->direction.y > 0.0);
    ORBIT_TEST_CHECK(
        topLeft->direction.z > 0.0);

    const auto bottomRight =
        orbit::render_view::ViewportRay(
            camera,
            200,
            100,
            1.0F,
            1.0F);

    ORBIT_TEST_CHECK(bottomRight.has_value());
    ORBIT_TEST_CHECK(
        bottomRight->direction.x > 0.0);
    ORBIT_TEST_CHECK(
        bottomRight->direction.y < 0.0);
    ORBIT_TEST_CHECK(
        bottomRight->direction.z > 0.0);

    ORBIT_TEST_CHECK(
        !orbit::render_view::ViewportRay(
            camera,
            0,
            100,
            0.5F,
            0.5F).
            has_value());

    return 0;
}
