#include <orbit/studio_ui/StudioFlatMap.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
using namespace orbit;
using namespace orbit::studio_ui;

void Check(const bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "Studio flat map test failed: " << what << "\n";
        std::exit(1);
    }
}

[[nodiscard]] bool Near(const f64 a, const f64 b, const f64 tolerance = 1.0e-9)
{
    return std::abs(a - b) <= tolerance;
}
} // namespace

int main()
{
    // Layer names round trip and unknown names are rejected.
    for (u32 index = 0U; index < kFlatMapLayerCount; ++index)
    {
        const auto layer = static_cast<FlatMapLayer>(index);
        const auto parsed = ParseFlatMapLayer(FlatMapLayerName(layer));
        Check(parsed.has_value() && *parsed == layer, "layer name round trip");
    }
    Check(ParseFlatMapLayer("tectonics") == FlatMapLayer::Tectonics,
          "tectonics layer is available");

    // Same convention as the text HUD: latitude = asin(y), longitude = atan2(z, x).
    {
        const auto pole = FlatMapLatLonFromDirection({0.0, 1.0, 0.0});
        Check(Near(pole.latitudeDegrees, 90.0), "north pole latitude");

        const auto east = FlatMapLatLonFromDirection({0.0, 0.0, 1.0});
        Check(Near(east.latitudeDegrees, 0.0), "equator latitude");
        Check(Near(east.longitudeDegrees, 90.0), "+Z is longitude 90");

        const auto prime = FlatMapLatLonFromDirection({1.0, 0.0, 0.0});
        Check(Near(prime.longitudeDegrees, 0.0), "+X is longitude 0");

        const auto direction = FlatMapDirectionFromLatLon(33.0, -120.0);
        const auto back = FlatMapLatLonFromDirection(direction);
        Check(Near(back.latitudeDegrees, 33.0, 1.0e-9) &&
                  Near(back.longitudeDegrees, -120.0, 1.0e-9),
              "lat/lon round trip");
        Check(Near(math::Length(direction), 1.0), "direction is a unit vector");
    }

    // Map image coordinates: north up, longitude -180 at the left.
    {
        const auto topLeft = FlatMapUvFromLatLon({90.0, -180.0});
        Check(Near(topLeft.x, 0.0) && Near(topLeft.y, 0.0), "top-left corner");
        const auto center = FlatMapUvFromLatLon({0.0, 0.0});
        Check(Near(center.x, 0.5) && Near(center.y, 0.5), "map center");
        const auto bottomRight = FlatMapLatLonFromUv({1.0, 1.0});
        Check(Near(bottomRight.latitudeDegrees, -90.0) &&
                  Near(bottomRight.longitudeDegrees, 180.0),
              "bottom-right corner");
    }

    // Letterboxing keeps the 2:1 aspect inside views of any shape.
    {
        const auto wide = FlatMapViewRect(2000U, 500U);
        Check(Near(wide.top, 0.0) && Near(wide.bottom, 1.0), "wide view fills height");
        Check(Near((wide.right - wide.left) * 2000.0, 1000.0),
              "wide view keeps 2:1");

        const auto tall = FlatMapViewRect(1000U, 1000U);
        Check(Near(tall.left, 0.0) && Near(tall.right, 1.0), "square view fills width");
        Check(Near((tall.bottom - tall.top) * 1000.0, 500.0),
              "square view keeps 2:1");

        // Centre of a letterboxed view is the centre of the map; the bars are not the map.
        const auto centre = FlatMapUvFromViewUv(1000U, 1000U, 0.5, 0.5);
        Check(centre.has_value() && Near(centre->x, 0.5) && Near(centre->y, 0.5),
              "view centre maps to map centre");
        Check(!FlatMapUvFromViewUv(1000U, 1000U, 0.5, 0.05).has_value(),
              "letterbox bar is not on the map");
        Check(Near(FlatMapViewRect(0U, 0U).right, 1.0), "degenerate view is harmless");
    }

    // Globe picking: a camera above the north pole looking down.
    {
        render_view::CameraState camera;
        camera.localPositionMeters = {0.0, 3.2 * 6'000'000.0, 0.0};
        camera.forward = {0.0F, -1.0F, 0.0F};
        camera.up = {0.0F, 0.0F, 1.0F};
        camera.verticalFovRadians = 1.04719755F;

        const auto center =
            GlobePickDirection(camera, 6'000'000.0, 1000U, 1000U, 0.5F, 0.5F);
        Check(center.has_value() && Near(center->y, 1.0, 1.0e-9),
              "centre of the globe view is the pole");

        // Far from the planet's disc the ray misses it.
        Check(!GlobePickDirection(camera, 6'000'000.0, 1000U, 1000U, 0.02F, 0.02F)
                   .has_value(),
              "corner of the view misses the planet");

        // Picked directions are unit vectors on the visible hemisphere.
        const auto edge =
            GlobePickDirection(camera, 6'000'000.0, 1000U, 1000U, 0.62F, 0.5F);
        Check(edge.has_value() && Near(math::Length(*edge), 1.0, 1.0e-9) &&
                  edge->y > 0.0,
              "off-centre pick is a unit vector facing the camera");
    }

    return 0;
}
