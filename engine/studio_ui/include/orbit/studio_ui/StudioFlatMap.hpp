#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/render_view/RenderView.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace orbit::rhi
{
class CommandList;
class Device;
class Texture;
} // namespace orbit::rhi

namespace orbit::shader
{
class Compiler;
}

namespace orbit::terrain
{
class TerrainSource;
}

namespace orbit::studio_ui
{
// What the flat map colours the planet by. Every layer is generated from the
// same terrain samples, so switching layers never re-samples the planet.
enum class FlatMapLayer : u8
{
    Elevation,
    Biomes,
    Temperature,
    Precipitation,
    WaterDepth,
    // Plates and every boundary influence on one map (the original overlay).
    Tectonics,
    // The three views below separate what the overlay mixes, so a feature can
    // be told apart from an artifact of one of them.
    // Plate identity only, with plate outlines.
    PlateId,
    // Relative plate motion at boundaries: convergence / divergence / shear.
    BoundaryMotion,
    // Distributed crustal deformation: stress and the fault network.
    CrustalDeformation
};

inline constexpr u32 kFlatMapLayerCount = 9U;

// Raster resolution of the map image (equirectangular, 2:1).
inline constexpr u32 kFlatMapWidth = 1024U;
inline constexpr u32 kFlatMapHeight = 512U;

[[nodiscard]] std::string_view FlatMapLayerName(FlatMapLayer layer) noexcept;
[[nodiscard]] std::optional<FlatMapLayer> ParseFlatMapLayer(
    std::string_view name) noexcept;

// Latitude/longitude use the same convention as the viewport text HUD:
// latitude = asin(direction.y) (+Y is the spin pole) and
// longitude = atan2(direction.z, direction.x), both in degrees.
struct FlatMapLatLon
{
    f64 latitudeDegrees{0.0};
    f64 longitudeDegrees{0.0};
};

[[nodiscard]] FlatMapLatLon FlatMapLatLonFromDirection(
    const math::Double3& direction) noexcept;
[[nodiscard]] math::Double3 FlatMapDirectionFromLatLon(
    f64 latitudeDegrees,
    f64 longitudeDegrees) noexcept;

// Normalized position inside the 2:1 map image (0..1 on both axes, origin
// top-left, north at the top, longitude -180 at the left).
[[nodiscard]] math::Double2 FlatMapUvFromLatLon(
    const FlatMapLatLon& latLon) noexcept;
[[nodiscard]] FlatMapLatLon FlatMapLatLonFromUv(
    const math::Double2& mapUv) noexcept;

// The map is letterboxed to keep its 2:1 aspect inside a view of any shape.
// The rectangle is in normalized view coordinates (0..1, origin top-left).
struct FlatMapRect
{
    f64 left{0.0};
    f64 top{0.0};
    f64 right{1.0};
    f64 bottom{1.0};
};

[[nodiscard]] FlatMapRect FlatMapViewRect(u32 viewWidth, u32 viewHeight) noexcept;

// View position (normalized, origin top-left) -> position inside the map
// image, or nullopt when the position falls on the letterbox bars.
[[nodiscard]] std::optional<math::Double2> FlatMapUvFromViewUv(
    u32 viewWidth,
    u32 viewHeight,
    f64 viewU,
    f64 viewV) noexcept;

// Picks the point of the planet under a viewport position in the globe
// (body_map) view: the unit direction, in the planet's body-fixed frame, of
// the first hit of the view ray with a sphere of the given radius centred on
// the body. nullopt when the position misses the planet. u and v are
// normalized (0..1, origin top-left).
[[nodiscard]] std::optional<math::Double3> GlobePickDirection(
    const render_view::CameraState& camera,
    f64 planetRadiusMeters,
    u32 viewWidth,
    u32 viewHeight,
    f32 u,
    f32 v) noexcept;

// What the flat map of one view is currently doing; exposed over RPC.
struct StudioFlatMapStatus
{
    FlatMapLayer layer{FlatMapLayer::Elevation};
    // True once the view has a terrain source to sample.
    bool hasSource{false};
    u32 rowsGenerated{0U};
    u32 rowsTotal{kFlatMapHeight};
    bool complete{false};
    // Where the observer of the view currently is.
    bool markerValid{false};
    FlatMapLatLon marker{};
};

// Generates and draws the flat planet map of Studio viewports in Flat Map
// mode. The raster is built on the main thread in small time slices (the
// terrain source is only valid while the runtime snapshot is current), uploaded
// to a per-view texture, and drawn after the output transform so exposure and
// tone mapping never touch it.
class StudioFlatMapRenderer
{
public:
    StudioFlatMapRenderer(
        rhi::Device& device,
        const shader::Compiler& compiler,
        u32 framesInFlight);
    ~StudioFlatMapRenderer();

    StudioFlatMapRenderer(const StudioFlatMapRenderer&) = delete;
    StudioFlatMapRenderer& operator=(const StudioFlatMapRenderer&) = delete;

    struct SourceBinding
    {
        const terrain::TerrainSource* terrain{nullptr};
        world::PlanetId planet{};
        f64 radiusMeters{0.0};
        // Changes whenever the planet's terrain changes; restarts generation.
        u64 revision{0U};
    };

    // Call once per frame, on the main thread, before the frame graph runs.
    // `source` may be null when the view has no current terrain runtime.
    void Advance(
        std::string_view viewId,
        const SourceBinding* source,
        FlatMapLayer layer,
        const std::optional<math::Double3>& markerDirection,
        f64 budgetMilliseconds);

    // Draws the map into `target` (the final RGBA8 display image of the
    // view), uploading new raster rows first when there are any.
    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& target,
        u32 width,
        u32 height,
        std::string_view viewId,
        u32 frameSlot);

    // Draws a lat/long graticule and the observer marker over the globe
    // (body_map) view, blended onto `target`.
    void DrawGlobeOverlay(
        rhi::CommandList& commands,
        rhi::Texture& target,
        u32 width,
        u32 height,
        const render_view::CameraState& camera,
        f64 planetRadiusMeters,
        const std::optional<math::Double3>& markerDirection);

    [[nodiscard]] std::optional<StudioFlatMapStatus> Status(
        std::string_view viewId) const;

    // True while any view is still generating its map, so the host keeps
    // rendering at full rate until it finishes.
    [[nodiscard]] bool Generating() const noexcept;

    void Forget(std::string_view viewId);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::studio_ui
