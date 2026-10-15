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

    // ProjectToViewport is the exact inverse of ViewportRay: a point on a
    // pixel's ray projects back to that pixel, for a tilted camera too.
    {
        orbit::render_view::CameraState tilted{
            .localPositionMeters = {5.0, 3.0, -20.0},
            .forward = {0.2F, -0.3F, 1.0F},
            .up = {0.0F, 1.0F, 0.0F},
            .verticalFovRadians = 1.0F
        };

        for (const float u : {0.1F, 0.5F, 0.85F})
        {
            for (const float v : {0.2F, 0.5F, 0.9F})
            {
                const auto ray =
                    orbit::render_view::ViewportRay(
                        tilted, 320, 180, u, v);
                ORBIT_TEST_CHECK(ray.has_value());

                const orbit::math::Double3 point{
                    ray->origin.x + ray->direction.x * 37.0,
                    ray->origin.y + ray->direction.y * 37.0,
                    ray->origin.z + ray->direction.z * 37.0};

                const auto projected =
                    orbit::render_view::ProjectToViewport(
                        tilted, 320, 180, point);
                ORBIT_TEST_CHECK(projected.has_value());
                ORBIT_TEST_CHECK(
                    Near(projected->u, u, 1.0e-5));
                ORBIT_TEST_CHECK(
                    Near(projected->v, v, 1.0e-5));
                ORBIT_TEST_CHECK(projected->depthMeters > 0.0);
            }
        }

        // Behind the camera and degenerate sizes do not project.
        ORBIT_TEST_CHECK(
            !orbit::render_view::ProjectToViewport(
                tilted, 320, 180, {5.0, 3.0, -40.0}).has_value());
        ORBIT_TEST_CHECK(
            !orbit::render_view::ProjectToViewport(
                tilted, 0, 180, {5.0, 3.0, 0.0}).has_value());
    }

    // Regression for Orbit's road/terrain camera-space contract.
    // The road shader must use the SAME up x forward screen-right as
    // LookAtLH, ViewportRay and ProjectToViewport; reversing the cross
    // mirrors a near-road quad into the wrong half of the viewport.
    {
        orbit::render_view::CameraState rolled{
            .localPositionMeters={1250.0,350.0,-620.0},
            .forward={0.45F,-0.22F,0.9F},
            .up={0.20F,1.0F,0.12F},
            .verticalFovRadians=1.1F,
            .nearPlaneMeters=0.05F,
            .farPlaneMeters=100000.0F
        };
        const orbit::math::Double3 forward=orbit::math::Normalize(
            orbit::math::Double3{
                rolled.forward.x,rolled.forward.y,rolled.forward.z});
        const orbit::math::Double3 up=orbit::math::Normalize(
            orbit::math::Double3{rolled.up.x,rolled.up.y,rolled.up.z});
        const auto right=orbit::math::Normalize(
            orbit::math::Cross(up,forward));
        const auto cameraUp=orbit::math::Normalize(
            orbit::math::Cross(forward,right));
        const double aspect=16.0/9.0;
        const double tanHalf=std::tan(rolled.verticalFovRadians*0.5);
        for (const auto lateral : {-0.8,0.0,0.7})
        {
            const auto relative=forward*60.0+
                right*(lateral*60.0)+cameraUp*6.0;
            const auto position=rolled.localPositionMeters+relative;
            const auto uv=orbit::render_view::ProjectToViewport(
                rolled,1600,900,position);
            ORBIT_TEST_CHECK(uv.has_value());
            const double projectedX=
                orbit::math::Dot(relative,right)/(60.0*aspect*tanHalf);
            const double projectedY=
                orbit::math::Dot(relative,cameraUp)/(60.0*tanHalf);
            // Negative Vulkan viewport height converts positive clip Y
            // into a smaller top-left-origin screen v coordinate.
            ORBIT_TEST_CHECK(Near(uv->u,0.5+projectedX*0.5,1.e-5));
            ORBIT_TEST_CHECK(Near(uv->v,0.5-projectedY*0.5,1.e-5));
        }
    }

    return 0;
}
