#include <orbit/studio_ui/StudioViewContinuity.hpp>

#include <orbit/math/Vector.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
constexpr u32 kVersion = 1U;

[[nodiscard]] std::optional<f64> ParseNumber(const std::string_view text)
{
    f64 value = 0.0;
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() ||
        !std::isfinite(value))
    {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<math::Double3> ParseVector(
    const std::string_view text)
{
    std::array<f64, 3> parts{};
    std::size_t begin = 0U;
    for (std::size_t index = 0U; index < parts.size(); ++index)
    {
        const std::size_t end =
            index + 1U < parts.size()
                ? text.find(' ', begin)
                : text.size();
        if (end == std::string_view::npos)
        {
            return std::nullopt;
        }
        const auto value = ParseNumber(text.substr(begin, end - begin));
        if (!value.has_value())
        {
            return std::nullopt;
        }
        parts[index] = *value;
        begin = end + 1U;
    }
    return math::Double3{parts[0], parts[1], parts[2]};
}

[[nodiscard]] std::string FormatVector(const math::Double3& value)
{
    return std::format("{} {} {}", value.x, value.y, value.z);
}

[[nodiscard]] bool IsUsableAxis(const math::Double3& axis) noexcept
{
    return math::LengthSquared(axis) > 1.0e-12;
}

// Fields are collected first so a pose is accepted or rejected as a unit.
struct PoseFields
{
    std::optional<std::string> target;
    bool hasCamera{true};
    bool terrain{false};
    f64 zoom{1.0};
    std::optional<math::Double3> observer;
    std::optional<math::Double3> east;
    std::optional<math::Double3> north;
    std::optional<math::Double3> up;
    std::optional<f64> yaw;
    std::optional<f64> pitch;

    [[nodiscard]] std::optional<StudioViewPose> Build() const
    {
        if (!target.has_value() || target->empty())
        {
            return std::nullopt;
        }

        StudioViewPose pose{};
        pose.targetObject = *target;
        pose.hasCamera = hasCamera;
        pose.terrain = terrain;
        pose.zoom = zoom;
        if (!hasCamera)
        {
            return pose;
        }

        if (!observer || !east || !north || !up || !yaw || !pitch ||
            !IsUsableAxis(*east) || !IsUsableAxis(*north) ||
            !IsUsableAxis(*up))
        {
            return std::nullopt;
        }

        pose.observerMeters = *observer;
        pose.surfaceFrame = {.east = *east, .north = *north, .up = *up};
        pose.yawRadians = *yaw;
        pose.pitchRadians = *pitch;
        return pose;
    }
};
} // namespace

std::string SerializeViewContinuity(
    const StudioViewContinuitySnapshot& snapshot)
{
    std::ostringstream output;
    output << "version=" << kVersion << '\n';

    if (snapshot.simulationMicroseconds.has_value())
    {
        output << "sim_us=" << *snapshot.simulationMicroseconds << '\n';
    }

    if (snapshot.pose.has_value())
    {
        const StudioViewPose& pose = *snapshot.pose;
        output << "target=" << pose.targetObject << '\n';
        output << "zoom=" << std::format("{}", pose.zoom) << '\n';
        output << "has_camera=" << (pose.hasCamera ? "1" : "0") << '\n';
        if (pose.hasCamera)
        {
            output << "terrain=" << (pose.terrain ? "1" : "0") << '\n';
            output << "observer=" << FormatVector(pose.observerMeters) << '\n';
            output << "east=" << FormatVector(pose.surfaceFrame.east) << '\n';
            output << "north=" << FormatVector(pose.surfaceFrame.north) << '\n';
            output << "up=" << FormatVector(pose.surfaceFrame.up) << '\n';
            output << "yaw=" << std::format("{}", pose.yawRadians) << '\n';
            output << "pitch=" << std::format("{}", pose.pitchRadians) << '\n';
        }
    }

    return output.str();
}

StudioViewContinuitySnapshot ParseViewContinuity(const std::string_view text)
{
    StudioViewContinuitySnapshot snapshot;
    PoseFields fields;

    std::istringstream input{std::string{text}};
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        const auto split = line.find('=');
        if (split == std::string::npos)
        {
            continue;
        }

        const std::string_view key{line.data(), split};
        const std::string_view value{
            line.data() + split + 1U, line.size() - split - 1U};

        if (key == "sim_us")
        {
            i64 microseconds = 0;
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), microseconds);
            if (parsed.ec == std::errc{} &&
                parsed.ptr == value.data() + value.size())
            {
                snapshot.simulationMicroseconds = microseconds;
            }
        }
        else if (key == "target") fields.target = std::string{value};
        else if (key == "has_camera") fields.hasCamera = value != "0";
        else if (key == "terrain") fields.terrain = value == "1";
        else if (key == "zoom")
        {
            if (const auto parsed = ParseNumber(value)) fields.zoom = *parsed;
        }
        else if (key == "observer") fields.observer = ParseVector(value);
        else if (key == "east") fields.east = ParseVector(value);
        else if (key == "north") fields.north = ParseVector(value);
        else if (key == "up") fields.up = ParseVector(value);
        else if (key == "yaw") fields.yaw = ParseNumber(value);
        else if (key == "pitch") fields.pitch = ParseNumber(value);
    }

    snapshot.pose = fields.Build();
    return snapshot;
}

StudioViewContinuitySnapshot LoadViewContinuity(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    return ParseViewContinuity(contents.str());
}

void SaveViewContinuity(
    const std::filesystem::path& path,
    const StudioViewContinuitySnapshot& snapshot)
{
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error(
                "Failed to open Studio view file for writing.");
        }
        output << SerializeViewContinuity(snapshot);
        if (!output)
        {
            throw std::runtime_error("Failed to write Studio view file.");
        }
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
        throw std::runtime_error("Failed to replace Studio view file.");
    }
}

StudioViewContinuity::StudioViewContinuity(std::string viewId)
    : viewId_(std::move(viewId))
{
}

StudioViewContinuity::~StudioViewContinuity()
{
    Flush();
}

void StudioViewContinuity::Bind(
    studio_session::StudioSession& session,
    const std::chrono::steady_clock::time_point now)
{
    // No flush here: when the root changes under the same session the world
    // already is the new project, so a capture would land in the old file.
    session_ = &session;
    path_ = session.World().Project().RootDirectory() / ".orbit" /
        "StudioView.ini";
    pending_ = LoadViewContinuity(path_);
    restored_ = false;
    timeApplied_ = false;
    restoreDeadline_ = now + kRestoreGrace;
    nextSave_ = now;
    lastWritten_.clear();

    // If the restore never lands, the on-disk camera must still outrank the
    // camera-less pose a half-loaded view reports (see Flush).
    if (pending_->pose.has_value() && pending_->pose->hasCamera)
    {
        lastWritten_ = SerializeViewContinuity(*pending_);
    }

    // Nothing remembered: nothing to wait for, start saving straight away.
    if (!pending_->pose.has_value() &&
        !pending_->simulationMicroseconds.has_value())
    {
        pending_.reset();
        restored_ = true;
    }
}

bool StudioViewContinuity::TryRestore(
    StudioRenderViewSet& views,
    studio_session::StudioSession& session)
{
    if (!pending_.has_value())
    {
        return true;
    }

    if (!timeApplied_)
    {
        if (pending_->simulationMicroseconds.has_value())
        {
            session.Clock().SetTime(
                time::SimulationTime{*pending_->simulationMicroseconds});
        }
        timeApplied_ = true;
    }

    if (!pending_->pose.has_value())
    {
        return true;
    }

    // Object IDs are minted per launch, so the saved target never matches the
    // reopened world. The saved camera belongs to the view's primary body:
    // adopt whatever that body is now.
    const auto current = views.ViewPose(viewId_);
    if (!current.has_value())
    {
        return false;
    }

    StudioViewPose pose = *pending_->pose;
    pose.targetObject = current->targetObject;

    // A terrain pose may only be restored once the terrain runtime is
    // current; otherwise RestoreViewPose would seed a reference-sphere
    // navigation entry for a body that is about to get real terrain.
    if (pose.hasCamera && pose.terrain &&
        !views.HasTerrainNavigation(viewId_))
    {
        return false;
    }

    return views.RestoreViewPose(viewId_, pose);
}

StudioViewContinuitySnapshot StudioViewContinuity::Capture(
    StudioRenderViewSet& views,
    studio_session::StudioSession& session)
{
    return {
        .pose = views.ViewPose(viewId_),
        .simulationMicroseconds =
            session.Clock().Time().microsecondsFromEpoch};
}

void StudioViewContinuity::Tick(
    StudioRenderViewSet& views,
    studio_session::StudioSession& session,
    const std::chrono::steady_clock::time_point now)
{
    if (!session.World().HasWorld())
    {
        return;
    }

    const auto root = session.World().Project().RootDirectory() / ".orbit" /
        "StudioView.ini";
    if (session_ != &session || root != path_)
    {
        Bind(session, now);
    }
    views_ = &views;

    if (!restored_)
    {
        if (TryRestore(views, session))
        {
            restored_ = true;
            pending_.reset();
        }
        else if (now >= restoreDeadline_)
        {
            restored_ = true;
            pending_.reset();
        }
        else
        {
            return;
        }
    }

    if (now < nextSave_)
    {
        return;
    }
    nextSave_ = now + kSaveInterval;
    Flush();
}

void StudioViewContinuity::Flush() noexcept
{
    if (!restored_ || session_ == nullptr || views_ == nullptr ||
        path_.empty())
    {
        return;
    }

    try
    {
        const auto snapshot = Capture(*views_, *session_);

        // No pose yet (no target body): nothing worth remembering.
        if (!snapshot.pose.has_value())
        {
            return;
        }

        // A terrain rebuild briefly reports a target without a camera; do not
        // let that erase a camera that was already saved.
        if (!snapshot.pose->hasCamera &&
            lastWritten_.find("has_camera=1") != std::string::npos)
        {
            return;
        }

        const std::string serialized = SerializeViewContinuity(snapshot);
        if (serialized == lastWritten_)
        {
            return;
        }

        SaveViewContinuity(path_, snapshot);
        lastWritten_ = serialized;
    }
    catch (...)
    {
        // Losing a view bookmark must never take Studio down.
    }
}
} // namespace orbit::studio_ui
