#include <orbit/mesh_render/GlassSurface.hpp>

#include <cmath>

namespace
{
int failures = 0;

void Check(const bool condition)
{
    if (!condition)
    {
        ++failures;
    }
}

bool Near(const float a, const float b, const float tolerance = 1.0e-4F)
{
    return std::abs(a - b) < tolerance;
}

orbit::lighting::LightingView ForwardZView()
{
    orbit::lighting::LightingView view;
    view.forward = {0.0F, 0.0F, 1.0F};
    view.up = {0.0F, 1.0F, 0.0F};
    view.verticalFovRadians = 1.5707964F; // 90 degrees: tan(half) = 1
    view.nearPlaneMeters = 0.1F;
    view.farPlaneMeters = 1000.0F;
    return view;
}

orbit::mesh_render::GlassInstance SphereAt(
    const float x, const float y, const float z, const float radius)
{
    orbit::mesh_render::GlassInstance instance;
    instance.rows = {1.0F, 0.0F, 0.0F, x,
                     0.0F, 1.0F, 0.0F, y,
                     0.0F, 0.0F, 1.0F, z};
    instance.shape = orbit::mesh_render::GlassShape::Sphere;
    instance.halfExtents = {radius, radius, radius};
    return instance;
}
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::mesh_render;

    // The camera basis matches the lighting passes (right = forward x up) and
    // the projection terms follow the field of view and aspect.
    {
        const auto camera = MakeGlassCamera(ForwardZView(), 200U, 100U);
        Check(Near(camera.forward[2], 1.0F));
        Check(Near(camera.up[1], 1.0F));
        Check(Near(camera.right[0], -1.0F)); // forward x up for +Z, +Y
        Check(Near(camera.tanHalfY, 1.0F));
        Check(Near(camera.tanHalfX, 2.0F));
        Check(Near(camera.nearPlane, 0.1F));
    }

    // A sphere straight ahead projects to a rectangle centred on the target.
    {
        const auto camera = MakeGlassCamera(ForwardZView(), 200U, 100U);
        const auto rect =
            GlassScreenBounds(SphereAt(0.0F, 0.0F, 10.0F, 1.0F), camera, 200U, 100U);
        Check(rect.has_value());
        if (rect.has_value())
        {
            Check(rect->left > 0 && rect->right < 200);
            Check(std::abs((rect->left + rect->right) / 2 - 100) <= 1);
            Check(std::abs((rect->top + rect->bottom) / 2 - 50) <= 1);
            // 2 m wide at 10 m with tan(half x) = 2: about 1/10 of the width.
            Check(rect->right - rect->left < 40);
        }
    }

    // A body behind the camera is off screen; one straddling the near plane
    // covers the whole target.
    {
        const auto camera = MakeGlassCamera(ForwardZView(), 200U, 100U);
        Check(!GlassScreenBounds(
                   SphereAt(0.0F, 0.0F, -10.0F, 1.0F), camera, 200U, 100U)
                   .has_value() ||
              GlassScreenBounds(
                  SphereAt(0.0F, 0.0F, -10.0F, 1.0F), camera, 200U, 100U)
                      ->right == 200);
        const auto straddling =
            GlassScreenBounds(SphereAt(0.0F, 0.0F, 0.0F, 1.0F), camera, 200U, 100U);
        Check(straddling.has_value());
        if (straddling.has_value())
        {
            Check(straddling->left == 0 && straddling->top == 0);
            Check(straddling->right == 200 && straddling->bottom == 100);
        }
    }

    // A body far to the side of the view is rejected.
    {
        const auto camera = MakeGlassCamera(ForwardZView(), 200U, 100U);
        Check(!GlassScreenBounds(
                   SphereAt(500.0F, 0.0F, 10.0F, 1.0F), camera, 200U, 100U)
                   .has_value());
    }

    // The packed record carries rows, tint + IOR, half extents + shape.
    {
        auto instance = SphereAt(1.0F, 2.0F, 3.0F, 0.5F);
        instance.shape = GlassShape::Capsule;
        instance.tint = {0.1F, 0.2F, 0.3F};
        instance.indexOfRefraction = 1.33F;
        instance.halfExtents = {0.4F, 1.0F, 0.4F};
        const auto record = PackGlassRecord(instance);
        Check(Near(record[3], 1.0F) && Near(record[7], 2.0F) &&
              Near(record[11], 3.0F));
        Check(Near(record[12], 0.1F) && Near(record[14], 0.3F) &&
              Near(record[15], 1.33F));
        Check(Near(record[17], 1.0F) && Near(record[19], 3.0F));
    }

    // The photon grid grows with the body but stays within its bounds.
    {
        Check(GlassPhotonGrid(0.0F) == GlassPhotonGrid(0.01F));
        Check(GlassPhotonGrid(0.5F) <= GlassPhotonGrid(1.0F));
        Check(GlassPhotonGrid(100.0F) == GlassPhotonGrid(1000.0F));
        Check(GlassPhotonGrid(1.0F) >= 96U && GlassPhotonGrid(1.0F) <= 224U);
    }

    return failures;
}
