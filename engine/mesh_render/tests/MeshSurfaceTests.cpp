#include <orbit/mesh_render/MeshSurface.hpp>

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

bool Near(const float a, const float b)
{
    return std::abs(a - b) < 1.0e-5F;
}
} // namespace

int main()
{
    using namespace orbit;

    // Identity rotation, scale 2, origin (1,2,3) relative to the camera.
    {
        const auto rows = mesh_render::MakeInstanceRows(
            math::Double3x3{}, 2.0, {1.0, 2.0, 3.0});
        Check(Near(rows[0], 2.0F) && Near(rows[5], 2.0F) && Near(rows[10], 2.0F));
        Check(Near(rows[3], 1.0F) && Near(rows[7], 2.0F) && Near(rows[11], 3.0F));
        Check(Near(rows[1], 0.0F) && Near(rows[4], 0.0F));
    }

    // A 90 degree turn about +Z sends model +X to scene +Y, so the first
    // column of the linear part is (0, s, 0).
    {
        math::Double3x3 rotation;
        rotation.xAxis = {0.0, 1.0, 0.0};
        rotation.yAxis = {-1.0, 0.0, 0.0};
        rotation.zAxis = {0.0, 0.0, 1.0};
        const auto rows = mesh_render::MakeInstanceRows(rotation, 3.0, {});
        // Row-major: element (row 1, column 0) is the scene Y of model X.
        Check(Near(rows[4], 3.0F));
        Check(Near(rows[0], 0.0F));
        Check(Near(rows[1], -3.0F));
    }

    return failures;
}
