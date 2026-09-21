#pragma once

#include <orbit/core/Types.hpp>

#include <optional>
#include <unordered_map>
#include <vector>

namespace orbit::celestial_scheduler
{
enum class WorkKind : u8
{
    OrbitalAppearance,
    AtmosphereLut,
    CloudField,
    FarImpostor,
    RingPresentation,
    AuroraPresentation,
    CompactPresentation,
    Count
};

enum class WorkBackend : u8
{
    Cpu,
    Gpu
};

struct WorkKey
{
    u64 subjectHigh{0U};
    u64 subjectLow{0U};
    WorkKind kind{WorkKind::OrbitalAppearance};

    [[nodiscard]] bool operator==(
        const WorkKey&) const noexcept = default;
};

struct WorkKeyHash
{
    [[nodiscard]] std::size_t operator()(
        const WorkKey& key) const noexcept;
};

struct WorkRequest
{
    WorkKey key{};

    // Opaque semantic/authority revision. It is compared for exact identity,
    // never numerically ordered.
    u64 authorityRevision{0U};

    WorkBackend backend{WorkBackend::Cpu};
    u32 costUnits{1U};

    // Higher values are scheduled first. Visible work receives a deterministic
    // bias but priority remains explicit so diagnostics/quality policy can tune it.
    i32 priority{0};
    bool visible{true};
};

struct WorkGrant
{
    WorkKey key{};
    u64 authorityRevision{0U};
    WorkBackend backend{WorkBackend::Cpu};
    u32 costUnits{0U};
};

struct SchedulerBudget
{
    u32 maxCpuJobsPerFrame{4U};
    u32 maxGpuJobsPerFrame{3U};
    u32 maxCpuCostUnitsPerFrame{8U};
    u32 maxGpuCostUnitsPerFrame{8U};

    // Hard queue cap prevents a newly viewed system from creating unbounded
    // derived work. Lowest-value deferred requests are discarded first.
    u32 maxPendingRequests{256U};
};

struct SchedulerFrameStats
{
    u32 cpuJobsGranted{0U};
    u32 gpuJobsGranted{0U};
    u32 cpuCostGranted{0U};
    u32 gpuCostGranted{0U};

    u32 pendingRequests{0U};
    u32 inFlightRequests{0U};
    u64 staleCompletionsRejected{0U};
    u64 queueDrops{0U};
};

class CelestialWorkScheduler
{
public:
    explicit CelestialWorkScheduler(
        SchedulerBudget budget = {});

    void SetBudget(
        SchedulerBudget budget);

    [[nodiscard]] const SchedulerBudget&
    Budget() const noexcept;

    // Enqueue is also the authority update. Re-enqueuing the same key with a
    // different revision replaces deferred work and makes any older in-flight
    // completion stale.
    void Enqueue(
        const WorkRequest& request);

    // Selects a bounded deterministic frame plan and marks grants in-flight.
    // A key with an older in-flight generation is not granted again until that
    // generation completes/rejects, preventing duplicate work explosions.
    [[nodiscard]] std::vector<WorkGrant>
    BuildFramePlan();

    // Returns true only when the completion still matches current authority.
    // Stale completions are consumed and counted but must not publish caches.
    [[nodiscard]] bool Complete(
        const WorkKey& key,
        u64 authorityRevision);

    // Releases a grant that became irrelevant before execution (for example a
    // viewport retarget). This is not a stale completion and does not publish.
    void Abandon(
        const WorkKey& key,
        u64 authorityRevision) noexcept;

    // Cancels/de-authorizes a subject/work pair. Any later completion for the
    // removed revision is stale.
    void Invalidate(
        const WorkKey& key);

    [[nodiscard]] std::optional<u64>
    CurrentAuthorityRevision(
        const WorkKey& key) const noexcept;

    [[nodiscard]] SchedulerFrameStats
    Stats() const noexcept;

private:
    struct PendingEntry
    {
        WorkRequest request{};
        u64 sequence{0U};
    };

    struct InFlightEntry
    {
        WorkGrant grant{};
    };

    void ValidateBudget(
        const SchedulerBudget& budget) const;
    void ValidateRequest(
        const WorkRequest& request) const;
    void TrimPending();

    SchedulerBudget budget_{};
    std::vector<PendingEntry> pending_;
    std::unordered_map<
        WorkKey,
        u64,
        WorkKeyHash>
        currentRevision_;
    std::unordered_map<
        WorkKey,
        InFlightEntry,
        WorkKeyHash>
        inFlight_;

    u64 sequence_{0U};
    u64 staleRejected_{0U};
    u64 queueDrops_{0U};
    SchedulerFrameStats lastFrame_{};
};
} // namespace orbit::celestial_scheduler
