#include <orbit/studio_ui/ViewportCaptureService.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <format>
#include <future>
#include <stdexcept>
#include <thread>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;

// Frames the renderer is given to converge at the capture size.
constexpr u32 kDefaultSettleFullscreen = 6U;
constexpr u32 kDefaultSettleUltra = 20U;
// Frames for each tile after the camera has been turned onto it.
constexpr u32 kTileSettleFrames = 8U;
// Longest waits, so a stuck view can never leave Studio stranded at a capture
// size.
constexpr u32 kResizeTimeoutFrames = 600U;
constexpr u32 kBusyTimeoutFrames = 600U;
constexpr u32 kRestoreTimeoutFrames = 600U;
// Neighbouring tiles are this fraction of a tile apart, so they overlap and
// bilinear sampling never reaches a tile edge.
constexpr f64 kTileStep = 0.875;

[[nodiscard]] u32 Even(const f64 value)
{
    const u32 rounded = static_cast<u32>(std::max(std::llround(value), 2LL));
    return rounded & ~1U;
}

struct Basis
{
    f64 forward[3];
    f64 right[3];
    f64 up[3];
};

[[nodiscard]] Basis TileBasis(const ViewportCaptureService::Tile& tile)
{
    const f64 sy = std::sin(tile.yawRadians);
    const f64 cy = std::cos(tile.yawRadians);
    const f64 sp = std::sin(tile.pitchRadians);
    const f64 cp = std::cos(tile.pitchRadians);
    Basis basis{};
    basis.forward[0] = cp * sy;
    basis.forward[1] = sp;
    basis.forward[2] = cp * cy;
    basis.right[0] = cy;
    basis.right[1] = 0.0;
    basis.right[2] = -sy;
    // up = forward x right.
    basis.up[0] =
        basis.forward[1] * basis.right[2] - basis.forward[2] * basis.right[1];
    basis.up[1] =
        basis.forward[2] * basis.right[0] - basis.forward[0] * basis.right[2];
    basis.up[2] =
        basis.forward[0] * basis.right[1] - basis.forward[1] * basis.right[0];
    return basis;
}

[[nodiscard]] f64 Dot3(const f64 (&a)[3], const f64 x, const f64 y, const f64 z)
{
    return a[0] * x + a[1] * y + a[2] * z;
}
} // namespace

ViewportCaptureService::ViewportCaptureService(
    Hooks hooks,
    std::filesystem::path directory)
    : hooks_(std::move(hooks)),
      directory_(std::move(directory))
{
}

std::string_view ViewportCaptureService::KindName(const Kind kind) noexcept
{
    switch (kind)
    {
    case Kind::Fullscreen:
        return "fullscreen";
    case Kind::Ultra:
        return "ultra";
    case Kind::Custom:
        return "custom";
    }
    return "custom";
}

ViewportCaptureService::Size ViewportCaptureService::TargetSize(
    const Request& request,
    const Size view,
    const Size window)
{
    switch (request.kind)
    {
    case Kind::Fullscreen:
        return {std::max(window.first, 2U) & ~1U,
                std::max(window.second, 2U) & ~1U};
    case Kind::Ultra:
    {
        const f64 width = static_cast<f64>(std::max(view.first, 1U));
        const f64 height = static_cast<f64>(std::max(view.second, 1U));
        if (width >= height)
        {
            return {kUltraLongSide,
                    std::min(Even(kUltraLongSide * height / width),
                             kMaxDimension)};
        }
        return {std::min(Even(kUltraLongSide * width / height), kMaxDimension),
                kUltraLongSide};
    }
    case Kind::Custom:
        return {request.width, request.height};
    }
    return view;
}

bool ViewportCaptureService::NeedsTiling(const Size target) noexcept
{
    return static_cast<u64>(target.first) * static_cast<u64>(target.second) >
        kMaxSinglePixels;
}

ViewportCaptureService::TilePlan ViewportCaptureService::PlanTiles(
    const Size target,
    const f64 verticalFovRadians)
{
    TilePlan plan;
    const f64 width = static_cast<f64>(target.first);
    const f64 height = static_cast<f64>(target.second);

    if (width >= height)
    {
        plan.tileSize = {
            std::min(kTileLongSide, target.first),
            Even(std::min<f64>(kTileLongSide, width) * height / width)};
    }
    else
    {
        plan.tileSize = {
            Even(std::min<f64>(kTileLongSide, height) * width / height),
            std::min(kTileLongSide, target.second)};
    }

    plan.focalPixels = (height / 2.0) / std::tan(verticalFovRadians / 2.0);

    const f64 tileWidth = static_cast<f64>(plan.tileSize.first);
    const f64 tileHeight = static_cast<f64>(plan.tileSize.second);
    const u32 cellWidth = std::max(1U, static_cast<u32>(tileWidth * kTileStep));
    const u32 cellHeight = std::max(1U, static_cast<u32>(tileHeight * kTileStep));
    const u32 columns = (target.first + cellWidth - 1U) / cellWidth;
    const u32 rows = (target.second + cellHeight - 1U) / cellHeight;

    // Each tile renders at the focal length of the output, so it is exactly
    // as sharp as the output where it is centred.
    const f64 tileFov =
        2.0 * std::atan((tileHeight / 2.0) / plan.focalPixels);

    for (u32 row = 0; row < rows; ++row)
    {
        for (u32 column = 0; column < columns; ++column)
        {
            TileCell cell;
            cell.x0 = column * cellWidth;
            cell.y0 = row * cellHeight;
            cell.x1 = std::min(cell.x0 + cellWidth, target.first);
            cell.y1 = std::min(cell.y0 + cellHeight, target.second);

            const f64 centerX = 0.5 * (cell.x0 + cell.x1);
            const f64 centerY = 0.5 * (cell.y0 + cell.y1);
            const f64 ax = (centerX - width / 2.0) / plan.focalPixels;
            const f64 ay = -(centerY - height / 2.0) / plan.focalPixels;

            // forward ~ (ax, ay, 1) = (tan yaw, tan pitch / cos yaw, 1).
            cell.camera.yawRadians = std::atan(ax);
            cell.camera.pitchRadians =
                std::atan(ay * std::cos(cell.camera.yawRadians));
            cell.camera.verticalFovRadians = tileFov;
            plan.cells.push_back(cell);
        }
    }
    return plan;
}

void ViewportCaptureService::CompositeTile(
    const TilePlan& plan,
    const TileCell& cell,
    const Size target,
    const render_view::CapturedImage& tile,
    std::vector<u8>& output)
{
    const Basis basis = TileBasis(cell.camera);
    const f64 halfWidth = target.first / 2.0;
    const f64 halfHeight = target.second / 2.0;
    const f64 focal = plan.focalPixels;
    const f64 tileCenterX = tile.width / 2.0;
    const f64 tileCenterY = tile.height / 2.0;
    const i64 maxX = static_cast<i64>(tile.width) - 1;
    const i64 maxY = static_cast<i64>(tile.height) - 1;

    const auto texel = [&tile](const i64 x, const i64 y, const int channel)
    {
        return static_cast<f64>(
            tile.rgba[(static_cast<std::size_t>(y) * tile.width +
                       static_cast<std::size_t>(x)) * 4U +
                      static_cast<std::size_t>(channel)]);
    };

    const auto rows = [&](const u32 beginRow, const u32 endRow)
    {
        for (u32 y = beginRow; y < endRow; ++y)
        {
            for (u32 x = cell.x0; x < cell.x1; ++x)
            {
                const f64 dx = (x + 0.5) - halfWidth;
                const f64 dy = -((y + 0.5) - halfHeight);
                const f64 w = Dot3(basis.forward, dx, dy, focal);
                const f64 u = Dot3(basis.right, dx, dy, focal);
                const f64 v = Dot3(basis.up, dx, dy, focal);

                // Continuous tile position; texel centres sit at +0.5.
                const f64 sx = tileCenterX + focal * u / w - 0.5;
                const f64 sy = tileCenterY - focal * v / w - 0.5;

                const f64 fx = std::floor(sx);
                const f64 fy = std::floor(sy);
                const f64 tx = sx - fx;
                const f64 ty = sy - fy;
                const i64 x0 = std::clamp<i64>(static_cast<i64>(fx), 0, maxX);
                const i64 y0 = std::clamp<i64>(static_cast<i64>(fy), 0, maxY);
                const i64 x1 =
                    std::clamp<i64>(static_cast<i64>(fx) + 1, 0, maxX);
                const i64 y1 =
                    std::clamp<i64>(static_cast<i64>(fy) + 1, 0, maxY);

                u8* out = output.data() +
                    (static_cast<std::size_t>(y) * target.first + x) * 4U;
                for (int channel = 0; channel < 4; ++channel)
                {
                    const f64 top = texel(x0, y0, channel) * (1.0 - tx) +
                        texel(x1, y0, channel) * tx;
                    const f64 bottom = texel(x0, y1, channel) * (1.0 - tx) +
                        texel(x1, y1, channel) * tx;
                    out[channel] = static_cast<u8>(
                        std::clamp(top * (1.0 - ty) + bottom * ty + 0.5,
                                   0.0,
                                   255.0));
                }
            }
        }
    };

    const u32 totalRows = cell.y1 - cell.y0;
    const u32 workers =
        std::clamp(std::thread::hardware_concurrency(), 1U, 16U);
    const u32 chunk = std::max(1U, (totalRows + workers - 1U) / workers);
    std::vector<std::future<void>> pending;
    for (u32 begin = cell.y0; begin < cell.y1; begin += chunk)
    {
        pending.push_back(std::async(
            std::launch::async,
            rows,
            begin,
            std::min(begin + chunk, cell.y1)));
    }
    for (auto& job : pending)
    {
        job.get();
    }
}

void ViewportCaptureService::Start(Request request)
{
    if (state_ != State::Idle)
    {
        throw std::logic_error("A viewport capture is already running.");
    }
    if (!hooks_.viewSize || !hooks_.windowSize || !hooks_.capture)
    {
        throw std::logic_error("Viewport capture is not attached.");
    }

    if (request.kind == Kind::Custom &&
        (request.width < 16U || request.height < 16U ||
         request.width > kMaxDimension || request.height > kMaxDimension))
    {
        throw std::invalid_argument(std::format(
            "Custom capture size must be between 16 and {} pixels.",
            kMaxDimension));
    }

    original_ = hooks_.viewSize();
    target_ = TargetSize(request, original_, hooks_.windowSize());
    if (target_.first == 0U || target_.second == 0U)
    {
        throw std::invalid_argument("The capture size is empty.");
    }

    tiled_ = NeedsTiling(target_);
    if (tiled_ &&
        (!hooks_.captureImage || !hooks_.setTile || !hooks_.viewFovRadians))
    {
        throw std::logic_error("Tiled capture is not attached.");
    }
    if (tiled_)
    {
        plan_ = PlanTiles(target_, hooks_.viewFovRadians());
        renderSize_ = plan_.tileSize;
    }
    else
    {
        renderSize_ = target_;
    }

    path_ = request.path;
    if (path_.empty())
    {
        const auto now = std::chrono::floor<std::chrono::seconds>(
            std::chrono::system_clock::now());
        path_ = directory_ /
            std::format(
                "{:%Y%m%d-%H%M%S}-{}-{}x{}.png",
                now,
                KindName(request.kind),
                target_.first,
                target_.second);
    }

    settleLeft_ = request.settleFrames != 0U
        ? request.settleFrames
        : (tiled_ ? kDefaultSettleUltra : kDefaultSettleFullscreen);
    busyWaited_ = 0U;
    stateFrames_ = 0U;
    tileIndex_ = 0U;
    result_.reset();
    error_.clear();
    request_ = std::move(request);
    state_ = State::Resizing;
}

std::filesystem::path ViewportCaptureService::OpenFolder()
{
    if (!hooks_.openPath)
    {
        throw std::logic_error("No file browser is attached.");
    }
    std::filesystem::create_directories(directory_);
    hooks_.openPath(directory_);
    return directory_;
}

bool ViewportCaptureService::Active() const noexcept
{
    return state_ != State::Idle;
}

std::optional<ViewportCaptureService::Size>
ViewportCaptureService::WantedViewSize() const
{
    switch (state_)
    {
    case State::Idle:
        return std::nullopt;
    case State::Resizing:
    case State::Settling:
    case State::TileSettling:
        return renderSize_;
    case State::Restoring:
        return original_;
    }
    return std::nullopt;
}

void ViewportCaptureService::ReleaseHolds()
{
    if (!holding_)
    {
        return;
    }
    holding_ = false;
    if (hooks_.setTile)
    {
        hooks_.setTile(std::nullopt);
    }
    if (hooks_.lockExposure)
    {
        hooks_.lockExposure(false);
    }
    if (hooks_.setSimulationPlaying)
    {
        hooks_.setSimulationPlaying(wasPlaying_);
    }
}

void ViewportCaptureService::Finish(std::string error)
{
    // Always put everything back, whatever happened.
    ReleaseHolds();
    output_.clear();
    output_.shrink_to_fit();
    error_ = std::move(error);
    stateFrames_ = 0U;
    state_ = State::Restoring;
}

void ViewportCaptureService::BeginTiles()
{
    wasPlaying_ =
        hooks_.simulationPlaying ? hooks_.simulationPlaying() : false;
    if (hooks_.setSimulationPlaying)
    {
        hooks_.setSimulationPlaying(false);
    }
    if (hooks_.lockExposure)
    {
        hooks_.lockExposure(true);
    }
    holding_ = true;
    output_.assign(
        static_cast<std::size_t>(target_.first) * target_.second * 4U, 0U);
    tileIndex_ = 0U;
    hooks_.setTile(plan_.cells.front().camera);
    settleLeft_ = kTileSettleFrames;
    busyWaited_ = 0U;
    state_ = State::TileSettling;
}

void ViewportCaptureService::ShootTile()
{
    const render_view::CapturedImage image = hooks_.captureImage();
    CompositeTile(plan_, plan_.cells[tileIndex_], target_, image, output_);

    ++tileIndex_;
    if (tileIndex_ < plan_.cells.size())
    {
        hooks_.setTile(plan_.cells[tileIndex_].camera);
        settleLeft_ = kTileSettleFrames;
        busyWaited_ = 0U;
        return;
    }

    result_ = render_view::WriteImageRgba8(
        path_, target_.first, target_.second, output_.data());
    Finish({});
}

void ViewportCaptureService::Tick()
{
    if (state_ == State::Idle)
    {
        return;
    }

    ++stateFrames_;
    const Size size = hooks_.viewSize();

    switch (state_)
    {
    case State::Idle:
        return;

    case State::Resizing:
        if (size == renderSize_)
        {
            state_ = State::Settling;
            stateFrames_ = 0U;
        }
        else if (stateFrames_ > kResizeTimeoutFrames)
        {
            Finish("The viewport did not resize to the capture size.");
        }
        return;

    case State::Settling:
        if (settleLeft_ > 0U)
        {
            --settleLeft_;
            return;
        }
        if (hooks_.busy && hooks_.busy() && busyWaited_ < kBusyTimeoutFrames)
        {
            ++busyWaited_;
            return;
        }
        try
        {
            if (tiled_)
            {
                BeginTiles();
            }
            else
            {
                result_ = hooks_.capture(path_);
                Finish({});
            }
        }
        catch (const std::exception& exception)
        {
            Finish(std::string("Capture failed: ") + exception.what());
        }
        return;

    case State::TileSettling:
        if (size != renderSize_)
        {
            Finish("The viewport changed size during the capture.");
            return;
        }
        if (settleLeft_ > 0U)
        {
            --settleLeft_;
            return;
        }
        if (hooks_.busy && hooks_.busy() && busyWaited_ < kBusyTimeoutFrames)
        {
            ++busyWaited_;
            return;
        }
        try
        {
            ShootTile();
        }
        catch (const std::exception& exception)
        {
            Finish(std::string("Capture failed: ") + exception.what());
        }
        return;

    case State::Restoring:
        if (size == original_ || stateFrames_ > kRestoreTimeoutFrames)
        {
            lastResult_ = result_;
            lastError_ = error_;
            lastKind_ = std::string(KindName(request_.kind));
            if (stateFrames_ > kRestoreTimeoutFrames && size != original_ &&
                lastError_.empty())
            {
                lastError_ =
                    "Captured, but the viewport did not return to its size.";
            }
            state_ = State::Idle;
        }
        return;
    }
}

Value ViewportCaptureService::Status() const
{
    const char* state = "idle";
    switch (state_)
    {
    case State::Idle:
        break;
    case State::Resizing:
        state = "resizing";
        break;
    case State::Settling:
        state = tiled_ ? "metering" : "settling";
        break;
    case State::TileSettling:
        state = "tiles";
        break;
    case State::Restoring:
        state = "restoring";
        break;
    }

    Value::Object status{
        {"state", std::string(state)},
        {"active", state_ != State::Idle}};
    if (state_ != State::Idle)
    {
        status.emplace("kind", std::string(KindName(request_.kind)));
        status.emplace("width", static_cast<i64>(target_.first));
        status.emplace("height", static_cast<i64>(target_.second));
        status.emplace("path", path_.generic_string());
        status.emplace("settle_frames_left", static_cast<i64>(settleLeft_));
        status.emplace("tiled", tiled_);
        if (tiled_)
        {
            status.emplace("tile", static_cast<i64>(tileIndex_));
            status.emplace("tiles", static_cast<i64>(plan_.cells.size()));
        }
    }

    if (lastResult_.has_value() || !lastError_.empty())
    {
        Value::Object last{{"kind", lastKind_}};
        if (lastResult_.has_value())
        {
            last.emplace("path", lastResult_->path.generic_string());
            last.emplace("width", static_cast<i64>(lastResult_->width));
            last.emplace("height", static_cast<i64>(lastResult_->height));
            last.emplace(
                "file_bytes", static_cast<i64>(lastResult_->fileBytes));
        }
        if (!lastError_.empty())
        {
            last.emplace("error", lastError_);
        }
        status.emplace("last", Value(std::move(last)));
    }
    return Value(std::move(status));
}

void RegisterViewportCaptureRpc(
    rpc::Dispatcher& dispatcher,
    ViewportCaptureService& service)
{
    dispatcher.Register(
        {
            .name = "viewport.capture_start",
            .description =
                "Starts a high-resolution capture of the primary viewport, "
                "the same as the viewport Screenshot / Ultra buttons. kind: "
                "'fullscreen' (window resolution), 'ultra' (16K: 15360 px "
                "on the long side, the viewport's aspect) or 'custom' "
                "(width, height up to 16384). Optional path (PNG, or BMP when it ends in .bmp) and "
                "settle_frames. The view is resized, given frames to "
                "converge, captured and restored, so this returns at once: "
                "poll viewport.capture_status until state is 'idle'.",
            .mutating = true
        },
        [&service](const Value& params)
        {
            ViewportCaptureService::Request request;
            if (params.IsObject())
            {
                const auto& object = params.AsObject();
                if (const auto kind = object.find("kind");
                    kind != object.end() && kind->second.IsString())
                {
                    const std::string& name = kind->second.AsString();
                    if (name == "fullscreen")
                    {
                        request.kind = ViewportCaptureService::Kind::Fullscreen;
                    }
                    else if (name == "ultra")
                    {
                        request.kind = ViewportCaptureService::Kind::Ultra;
                    }
                    else if (name == "custom")
                    {
                        request.kind = ViewportCaptureService::Kind::Custom;
                    }
                    else
                    {
                        throw rpc::Error(
                            -32602,
                            "kind must be fullscreen, ultra or custom.");
                    }
                }
                const auto number = [&object](const char* key) -> u32
                {
                    const auto found = object.find(key);
                    if (found == object.end() || !found->second.IsNumber())
                    {
                        return 0U;
                    }
                    const f64 value = found->second.AsNumber();
                    if (!(value >= 0.0) || value > 1.0e6)
                    {
                        throw rpc::Error(
                            -32602,
                            std::string(key) + " is out of range.");
                    }
                    return static_cast<u32>(value);
                };
                request.width = number("width");
                request.height = number("height");
                request.settleFrames = number("settle_frames");
                if (const auto path = object.find("path");
                    path != object.end() && path->second.IsString())
                {
                    request.path = path->second.AsString();
                }
            }

            try
            {
                service.Start(std::move(request));
            }
            catch (const std::invalid_argument& exception)
            {
                throw rpc::Error(-32602, exception.what());
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1110, exception.what());
            }
            return service.Status();
        });

    dispatcher.Register(
        {
            .name = "viewport.screenshots_open",
            .description =
                "Opens the folder screenshots are saved to (<project>/Screenshots) "
                "in the platform file browser, the same as the viewport "
                "Screenshot Files button. Returns its path.",
            .mutating = true
        },
        [&service](const Value&)
        {
            try
            {
                return Value(Value::Object{
                    {"path", service.OpenFolder().generic_string()}});
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(1111, exception.what());
            }
        });

    dispatcher.Register(
        {
            .name = "viewport.capture_status",
            .description =
                "State of the high-resolution capture: idle, resizing, "
                "settling or restoring, and the outcome of the last one "
                "(path, width, height, file_bytes or error).",
            .mutating = false
        },
        [&service](const Value&)
        {
            return service.Status();
        });
}
} // namespace orbit::studio_ui
