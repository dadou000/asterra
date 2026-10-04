#include <orbit/studio_ui/ViewportCaptureService.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
using orbit::studio_ui::ViewportCaptureService;

int g_failures = 0;

void Expect(const bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << what << "\n";
        ++g_failures;
    }
}

// A smooth scene that depends only on the viewing direction (x right, y up,
// z forward), like a sky: any correct reprojection reproduces it.
unsigned char Scene(const double x, const double y, const double z, const int c)
{
    const double length = std::sqrt(x * x + y * y + z * z);
    const double a = x / length;
    const double b = y / length;
    const double d = z / length;
    const double value = c == 0 ? 0.5 + 0.5 * a
                       : c == 1 ? 0.5 + 0.5 * b
                       : c == 2 ? 0.5 + 0.5 * d
                                : 1.0;
    return static_cast<unsigned char>(std::lround(value * 255.0));
}

// Renders a tile of the scene the way a pinhole camera turned by cell.camera
// would see it.
orbit::render_view::CapturedImage RenderTile(
    const ViewportCaptureService::TilePlan& plan,
    const ViewportCaptureService::TileCell& cell)
{
    const double sy = std::sin(cell.camera.yawRadians);
    const double cy = std::cos(cell.camera.yawRadians);
    const double sp = std::sin(cell.camera.pitchRadians);
    const double cp = std::cos(cell.camera.pitchRadians);
    const double f[3] = {cp * sy, sp, cp * cy};
    const double r[3] = {cy, 0.0, -sy};
    const double u[3] = {
        f[1] * r[2] - f[2] * r[1],
        f[2] * r[0] - f[0] * r[2],
        f[0] * r[1] - f[1] * r[0]};

    orbit::render_view::CapturedImage image;
    image.width = plan.tileSize.first;
    image.height = plan.tileSize.second;
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4U);
    for (unsigned y = 0; y < image.height; ++y)
    {
        for (unsigned x = 0; x < image.width; ++x)
        {
            const double px = (x + 0.5 - image.width / 2.0) / plan.focalPixels;
            const double py = -(y + 0.5 - image.height / 2.0) / plan.focalPixels;
            double d[3];
            for (int k = 0; k < 3; ++k)
            {
                d[k] = r[k] * px + u[k] * py + f[k];
            }
            for (int c = 0; c < 4; ++c)
            {
                image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4U + c] =
                    Scene(d[0], d[1], d[2], c);
            }
        }
    }
    return image;
}
} // namespace

int main()
{
    // Planning: every output pixel belongs to exactly one tile.
    const ViewportCaptureService::Size target{960, 540};
    const double fov = 70.0 * 3.14159265358979 / 180.0;
    const auto plan = ViewportCaptureService::PlanTiles(target, fov);
    Expect(plan.cells.size() > 1, "a large output needs several tiles");

    std::vector<int> covered(static_cast<std::size_t>(target.first) * target.second, 0);
    for (const auto& cell : plan.cells)
    {
        for (unsigned y = cell.y0; y < cell.y1; ++y)
        {
            for (unsigned x = cell.x0; x < cell.x1; ++x)
            {
                ++covered[static_cast<std::size_t>(y) * target.first + x];
            }
        }
    }
    bool exact = true;
    for (const int count : covered)
    {
        exact = exact && count == 1;
    }
    Expect(exact, "tile cells partition the output");

    // Reprojection: the stitched image equals the scene seen by the original
    // camera, to within quantisation and bilinear error.
    std::vector<unsigned char> output(
        static_cast<std::size_t>(target.first) * target.second * 4U, 0);
    for (const auto& cell : plan.cells)
    {
        ViewportCaptureService::CompositeTile(
            plan, cell, target, RenderTile(plan, cell), output);
    }

    int worst = 0;
    for (unsigned y = 0; y < target.second; ++y)
    {
        for (unsigned x = 0; x < target.first; ++x)
        {
            const double dx = x + 0.5 - target.first / 2.0;
            const double dy = -(y + 0.5 - target.second / 2.0);
            for (int c = 0; c < 4; ++c)
            {
                const int expected = Scene(dx, dy, plan.focalPixels, c);
                const int actual =
                    output[(static_cast<std::size_t>(y) * target.first + x) * 4U + c];
                worst = std::max(worst, std::abs(expected - actual));
            }
        }
    }
    std::cout << "tiles " << plan.cells.size() << " worst error " << worst << "\n";
    Expect(worst <= 6, "stitched tiles reproduce the scene");

    Expect(ViewportCaptureService::NeedsTiling({15360, 8640}), "16K is tiled");
    Expect(!ViewportCaptureService::NeedsTiling({3840, 2160}), "4K is not");

    const auto ultra = ViewportCaptureService::TargetSize(
        {ViewportCaptureService::Kind::Ultra}, {1600, 900}, {2560, 1440});
    Expect(ultra.first == 15360 && ultra.second == 8640, "ultra keeps 16:9");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
