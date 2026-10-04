#pragma once

#include <orbit/studio_ui/StudioRenderViewSet.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace orbit::studio_ui
{
// What Studio remembers about "where you were" in a project: the primary view's
// camera pose and the simulation time. Presentation state only; it never
// participates in world authority.
struct StudioViewContinuitySnapshot
{
    std::optional<StudioViewPose> pose;
    std::optional<i64> simulationMicroseconds;
};

[[nodiscard]] std::string SerializeViewContinuity(
    const StudioViewContinuitySnapshot& snapshot);

// Unknown keys are ignored. A pose with a missing or non-finite field is
// dropped whole rather than half-applied.
[[nodiscard]] StudioViewContinuitySnapshot ParseViewContinuity(
    std::string_view text);

[[nodiscard]] StudioViewContinuitySnapshot LoadViewContinuity(
    const std::filesystem::path& path);

void SaveViewContinuity(
    const std::filesystem::path& path,
    const StudioViewContinuitySnapshot& snapshot);

// Saves the primary view's pose and the simulation time to
// <project>/.orbit/StudioView.ini and puts them back the next time the project
// opens. Tick it once per frame; the destructor performs a final save.
//
// Saving stays off until the restore has been attempted so a freshly opened
// project (default camera) can never overwrite the remembered one. The restore
// waits for the terrain runtime to be current, and gives up after kRestoreGrace
// so a body that no longer exists cannot block saving forever.
class StudioViewContinuity
{
public:
    static constexpr std::chrono::seconds kSaveInterval{1};
    static constexpr std::chrono::seconds kRestoreGrace{30};

    explicit StudioViewContinuity(std::string viewId = "studio.primary");
    ~StudioViewContinuity();

    StudioViewContinuity(const StudioViewContinuity&) = delete;
    StudioViewContinuity& operator=(const StudioViewContinuity&) = delete;

    void Tick(
        StudioRenderViewSet& views,
        studio_session::StudioSession& session,
        std::chrono::steady_clock::time_point now);

    // Saves immediately if anything changed. Safe to call at any time.
    void Flush() noexcept;

private:
    void Bind(
        studio_session::StudioSession& session,
        std::chrono::steady_clock::time_point now);
    [[nodiscard]] bool TryRestore(
        StudioRenderViewSet& views,
        studio_session::StudioSession& session);
    [[nodiscard]] StudioViewContinuitySnapshot Capture(
        StudioRenderViewSet& views,
        studio_session::StudioSession& session);

    std::string viewId_;
    studio_session::StudioSession* session_{nullptr};
    StudioRenderViewSet* views_{nullptr};
    std::filesystem::path path_;
    std::optional<StudioViewContinuitySnapshot> pending_;
    bool restored_{false};
    bool timeApplied_{false};
    std::chrono::steady_clock::time_point restoreDeadline_{};
    std::chrono::steady_clock::time_point nextSave_{};
    std::string lastWritten_;
};
} // namespace orbit::studio_ui
