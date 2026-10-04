#pragma once

#include <orbit/render_view/Capture.hpp>
#include <orbit/rpc/JsonRpc.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orbit::studio_ui
{
// High-resolution captures of the primary viewport.
//
// Up to 8K the view is simply resized to the capture size, given a few frames
// to settle (temporal filtering, terrain streaming, lighting caches) and
// captured. Anything larger (the 16K Ultra shot) does not fit in GPU memory as
// one render, so it is tiled: the camera is turned onto a grid of tiles with a
// narrower field of view, each tile is rendered at 4K with exposure and the
// simulation clock held still, and the tiles are reprojected into one large
// image. Turning a pinhole camera about its centre changes no perspective, so
// the stitch has no seams or parallax; very wide fields of view are a little
// softer towards the corners.
//
// The host loop applies WantedViewSize() and calls Tick() once per frame at
// the point where the previous frame has completed. The panel buttons and the
// viewport.capture_* RPC / MCP methods both call Start(), so a button and an
// agent do exactly the same thing.
class ViewportCaptureService
{
public:
    using Size = std::pair<u32, u32>;

    // A camera turned by yaw then pitch with a new vertical field of view.
    struct Tile
    {
        f64 yawRadians{0.0};
        f64 pitchRadians{0.0};
        f64 verticalFovRadians{1.0};
    };

    struct Hooks
    {
        // Current size of the primary RenderView.
        std::function<Size()> viewSize;
        // Client size of the Studio window: what a fullscreen viewport would
        // be.
        std::function<Size()> windowSize;
        std::function<render_view::CaptureResult(
            const std::filesystem::path&)> capture;
        // The view's vertical field of view now (zoom included).
        std::function<f64()> viewFovRadians;
        std::function<render_view::CapturedImage()> captureImage;
        // Turns the camera for a tile; nullopt returns to the normal camera.
        std::function<void(const std::optional<Tile>&)> setTile;
        // Holds the exposure still while tiles are shot.
        std::function<void(bool)> lockExposure;
        // The simulation clock: held still while tiles are shot.
        std::function<bool()> simulationPlaying;
        std::function<void(bool)> setSimulationPlaying;
        // True while the renderer still has terrain/streaming work pending.
        std::function<bool()> busy;
        // Shows a folder or file in the platform file browser.
        std::function<void(const std::filesystem::path&)> openPath;
    };

    enum class Kind
    {
        // The viewport at the full window resolution, without the panel
        // chrome around it.
        Fullscreen,
        // 16K on the long side (15360 px), keeping the viewport's aspect.
        Ultra,
        Custom
    };

    struct Request
    {
        Kind kind{Kind::Fullscreen};
        // Custom only.
        u32 width{0};
        u32 height{0};
        // Empty: <directory>/<time>-<kind>-<w>x<h>.png.
        std::filesystem::path path;
        // 0: a default that suits the kind.
        u32 settleFrames{0};
    };

    inline static constexpr u32 kUltraLongSide = 15'360U;
    inline static constexpr u32 kMaxDimension = 16'384U;
    // Largest capture rendered in one piece (7680x4320).
    inline static constexpr u64 kMaxSinglePixels = 7'680ULL * 4'320ULL;
    // Long side of one tile of a tiled capture.
    inline static constexpr u32 kTileLongSide = 3'840U;

    ViewportCaptureService(Hooks hooks, std::filesystem::path directory);

    // Throws std::invalid_argument for a bad request and std::logic_error when
    // a capture is already running.
    void Start(Request request);

    [[nodiscard]] bool Active() const noexcept;

    // The folder screenshots are saved to by default.
    [[nodiscard]] const std::filesystem::path& Directory() const noexcept
    {
        return directory_;
    }

    // Shows that folder in the file browser (creating it when nothing has been
    // captured yet). Throws when no file browser is attached.
    std::filesystem::path OpenFolder();

    // The size the primary view must have right now, if a capture needs it
    // different from the panel's.
    [[nodiscard]] std::optional<Size> WantedViewSize() const;

    void Tick();

    [[nodiscard]] rpc::Value Status() const;

    [[nodiscard]] static std::string_view KindName(Kind kind) noexcept;
    // Size a request would capture at, for the given current view/window.
    [[nodiscard]] static Size TargetSize(
        const Request& request,
        Size view,
        Size window);
    [[nodiscard]] static bool NeedsTiling(Size target) noexcept;

    struct TileCell
    {
        // Pixels of the output this tile is responsible for.
        u32 x0{0}, y0{0}, x1{0}, y1{0};
        Tile camera;
    };
    struct TilePlan
    {
        Size tileSize{1U, 1U};
        // Focal length in output pixels.
        f64 focalPixels{1.0};
        std::vector<TileCell> cells;
    };
    // The grid of tiles that covers an output of `target` seen with the given
    // vertical field of view.
    [[nodiscard]] static TilePlan PlanTiles(
        Size target,
        f64 verticalFovRadians);

    // Reprojects one rendered tile (rendered with the camera of `cell` and
    // `plan`) into the cell's pixels of `output` (target.first * target.second
    // RGBA8, top-down).
    static void CompositeTile(
        const TilePlan& plan,
        const TileCell& cell,
        Size target,
        const render_view::CapturedImage& tile,
        std::vector<u8>& output);

private:
    enum class State
    {
        Idle,
        Resizing,
        // Single capture: frames to converge. Tiled capture: frames to meter
        // the exposure before it is locked.
        Settling,
        // Tiled capture: frames for the current tile.
        TileSettling,
        Restoring
    };

    void BeginTiles();
    void ShootTile();
    void Finish(std::string error);
    void ReleaseHolds();

    Hooks hooks_;
    std::filesystem::path directory_;

    State state_{State::Idle};
    Request request_;
    Size original_{1U, 1U};
    Size target_{1U, 1U};
    // What the view is resized to while capturing (the tile size when tiled).
    Size renderSize_{1U, 1U};
    std::filesystem::path path_;
    u32 settleLeft_{0};
    u32 busyWaited_{0};
    u32 stateFrames_{0};
    std::optional<render_view::CaptureResult> result_;
    std::string error_;

    bool tiled_{false};
    TilePlan plan_;
    std::size_t tileIndex_{0};
    std::vector<u8> output_;
    bool wasPlaying_{false};
    bool holding_{false};

    // The outcome of the last capture, kept for Status().
    std::optional<render_view::CaptureResult> lastResult_;
    std::string lastError_;
    std::string lastKind_;
};

// viewport.capture_start / viewport.capture_status.
void RegisterViewportCaptureRpc(
    rpc::Dispatcher& dispatcher,
    ViewportCaptureService& service);
} // namespace orbit::studio_ui
