#pragma once

// Helpers the Orbit Studio composition root (Main.cpp) uses: path-network queries, project and
// body bootstrap, id encoding, plugin panel sync, RPC notification toasts and the per-frame CPU
// telemetry. Moved out of Main.cpp so edits to them recompile a small translation unit.

#include <orbit/content/ContentService.hpp>
#include <orbit/core/Log.hpp>
#include <orbit/documents/ProjectDocument.hpp>
#include <orbit/editor_ui/BodyPreviewRenderer.hpp>
#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/path_geometry/PathDerived.hpp>
#include <orbit/path_routing/RoutePlanner.hpp>
#include <orbit/paths/PathNetwork.hpp>
#include <orbit/plugins/PluginManager.hpp>
#include <orbit/profiler/Profiler.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/scene/ObjectStore.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace orbit::editor_app::support
{
struct CpuFrameTelemetry
{
    static constexpr std::size_t kWindow = 120U;
    static constexpr std::size_t kPhaseCount = 23U;

    enum Phase : std::size_t
    {
        FenceWait,
        PreUi,
        Ui,
        SceneUpdate,
        RenderGraphSetup,
        RenderGraphExecute,
        ViewportCompose,
        ComposeEarly,
        ComposeCelestial,
        ComposeTerrain,
        ComposeRender,
        ComposeGi,
        ComposeAtmosphere,
        ComposePost,
        GiPrepare,
        GiUpdateList,
        GiEstimate,
        GiSnapshot,
        GiSnapshotBuild,
        GiSnapshotUpload,
        GiPasses,
        SubmitPresent,
        WholeLoop
    };

    std::array<std::array<std::atomic<double>, kWindow>, kPhaseCount>
        samples{};
    std::array<std::atomic<double>, kPhaseCount> rollingSums{};
    std::array<std::atomic<double>, kPhaseCount> lastMs{};
    std::atomic<orbit::u64> frameCount{0U};
    std::atomic<orbit::u64> radianceUpdatesLastFrame{0U};

    void Record(const Phase phase, const double milliseconds) noexcept
    {
        const auto frame = frameCount.load(std::memory_order_relaxed);
        const auto slot = static_cast<std::size_t>(frame % kWindow);
        const auto previous = samples[phase][slot].exchange(
            milliseconds,
            std::memory_order_relaxed);
        rollingSums[phase].fetch_add(
            milliseconds - previous,
            std::memory_order_relaxed);
        lastMs[phase].store(milliseconds, std::memory_order_relaxed);

        // Mirror the phase into the micro-profiler as a synthetic lane so a hitch
        // capture shows which phase of the main loop ate the time. Phases that
        // nest inside others (compose_*, gi_*) go on their own lane.
        if (orbit::profiler::Enabled() && phase != WholeLoop)
        {
            static constexpr std::array<const char*, kPhaseCount> labels{
                "fence_wait", "pre_ui", "ui", "scene_update",
                "render_graph_setup", "render_graph_execute",
                "viewport_compose", "compose_early",
                "compose_celestial", "compose_terrain", "compose_render",
                "compose_gi", "compose_atmosphere", "compose_post",
                "gi_prepare", "gi_update_list", "gi_estimate",
                "gi_snapshot", "gi_snapshot_build", "gi_snapshot_upload",
                "gi_passes",
                "submit_present", "whole_loop"};
            const bool topLevel =
                phase == FenceWait || phase == PreUi || phase == Ui ||
                phase == SceneUpdate || phase == RenderGraphSetup ||
                phase == RenderGraphExecute || phase == ViewportCompose ||
                phase == SubmitPresent;
            const orbit::u64 end = orbit::profiler::NowTicks();
            const auto duration = static_cast<orbit::u64>(
                std::max(milliseconds, 0.0) *
                orbit::profiler::TicksPerMillisecond());
            orbit::profiler::RecordLaneSpan(
                topLevel ? "Main loop phases" : "Main loop sub-phases",
                labels[phase],
                end > duration ? end - duration : 0U,
                end);
        }
    }

    void FinishFrame() noexcept
    {
        frameCount.fetch_add(1U, std::memory_order_relaxed);
    }

    void RecordRadianceUpdates(const orbit::u32 count) noexcept
    {
        radianceUpdatesLastFrame.store(
            count,
            std::memory_order_relaxed);
    }

    [[nodiscard]] double RollingAverageMs(const Phase phase) const noexcept
    {
        const auto frames =
            std::min<orbit::u64>(
                frameCount.load(std::memory_order_relaxed),
                kWindow);
        return frames == 0U
            ? 0.0
            : rollingSums[phase].load(std::memory_order_relaxed) /
                  static_cast<double>(frames);
    }

    [[nodiscard]] orbit::rpc::Value Snapshot() const
    {
        const auto frames = frameCount.load(std::memory_order_relaxed);
        const auto windowFrames = std::min<orbit::u64>(frames, kWindow);
        static constexpr std::array<const char*, kPhaseCount> names{
            "fence_wait", "pre_ui", "ui", "scene_update",
            "render_graph_setup", "render_graph_execute",
            "viewport_compose", "compose_early",
            "compose_celestial", "compose_terrain", "compose_render",
            "compose_gi", "compose_atmosphere", "compose_post",
            "gi_prepare", "gi_update_list", "gi_estimate",
            "gi_snapshot", "gi_snapshot_build", "gi_snapshot_upload",
            "gi_passes",
            "submit_present", "whole_loop"};
        orbit::rpc::Value::Object timings;
        for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
        {
            timings.emplace(
                names[phase],
                orbit::rpc::Value(orbit::rpc::Value::Object{
                    {"last_ms", lastMs[phase].load(std::memory_order_relaxed)},
                    {"rolling_120_frame_average_ms",
                     windowFrames == 0U
                         ? 0.0
                         : rollingSums[phase].load(std::memory_order_relaxed) /
                               static_cast<double>(windowFrames)}}));
        }
        return orbit::rpc::Value(orbit::rpc::Value::Object{
            {"frames", static_cast<orbit::i64>(frames)},
            {"window_frames", static_cast<orbit::i64>(windowFrames)},
            {"radiance_updates_last_frame",
             static_cast<orbit::i64>(radianceUpdatesLastFrame.load(
                 std::memory_order_relaxed))},
            {"timings_ms", orbit::rpc::Value(std::move(timings))}});
    }
};

struct ResolvedRoutingProfile
{
    orbit::paths::PathProfile profile;
    orbit::u64 revision{1};
};

[[nodiscard]] ResolvedRoutingProfile
ResolveRoutingProfile(
    const orbit::paths::PathEdgeRecord& edge,
    orbit::paths::PathNetworkService& paths,
    const orbit::content::ContentService& content,
    const std::filesystem::path& projectRoot);

[[nodiscard]] std::vector<
    orbit::scene::ObjectId>
FindRoutedPathEdges(
    orbit::scene::ObjectStore& objects,
    orbit::paths::PathNetworkService& paths);

[[nodiscard]] std::vector<
    orbit::scene::ObjectId>
FindPathEdges(
    orbit::scene::ObjectStore& objects,
    orbit::paths::PathNetworkService& paths);

[[nodiscard]] std::optional<
    orbit::frames::FrameId>
PathAnchorNativeFrame(
    const orbit::paths::PathAnchor& anchor,
    const orbit::universe::BodyRegistry& bodies);

[[nodiscard]]
orbit::path_routing::RouteSearchConfig
RouteSearchForProfile(
    const orbit::paths::PathProfile& profile);

[[nodiscard]] std::optional<
    orbit::documents::ProjectDocument>
OpenProject(
    const int argc,
    char** argv);

[[nodiscard]] const char* LogPrefix(
    const orbit::log::Level level) noexcept;

[[nodiscard]] std::optional<
    orbit::scene::ObjectId>
FindFirstBodyObject(
    orbit::scene::ObjectStore& objects);

[[nodiscard]] orbit::scene::ObjectId
EnsureInitialBodyObject(
    orbit::scene::ObjectStore& objects,
    orbit::commands::CommandService& commands);

[[nodiscard]] std::array<
    std::byte,
    sizeof(orbit::scene::ObjectId)>
EncodeObjectId(
    const orbit::scene::ObjectId id);

[[nodiscard]] std::optional<
    orbit::scene::ObjectId>
DecodeObjectId(
    const std::vector<std::byte>& bytes);

[[nodiscard]] std::array<
    std::byte,
    sizeof(orbit::content::AssetId)>
EncodeAssetId(
    const orbit::content::AssetId id);

[[nodiscard]] std::optional<
    orbit::content::AssetId>
DecodeAssetId(
    const std::vector<std::byte>& bytes);

void SynchronizePluginPanels(
    orbit::editor_ui::EditorUi& ui,
    const std::function<
        orbit::plugins::PluginManager&()>&
        plugins,
    std::vector<orbit::editor_ui::PanelId>&
        registered);

[[nodiscard]] orbit::editor_ui::PreviewMaterial
PreviewMaterialForAsset(
    orbit::content::ContentService& content,
    const orbit::content::AssetRecord* asset,
    const orbit::u32 depth = 0);

[[nodiscard]] std::filesystem::path
FindPlayerExecutable();

void RecordRpcNotifications(
    const orbit::rpc::Dispatcher& dispatcher,
    const std::string_view message,
    const std::optional<std::string>& response,
    std::vector<orbit::editor_ui::EditorUi::Notification>& out);
} // namespace orbit::editor_app::support
