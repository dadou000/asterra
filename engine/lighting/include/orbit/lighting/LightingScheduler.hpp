#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace orbit::lighting
{
enum class LightingGpuSection : u8
{
    Direct,
    Visibility,
    Gi,
    Reflections,
    Emissive,
    PostProcess,
    Count
};

inline constexpr u32 kLightingGpuSectionCount =
    static_cast<u32>(LightingGpuSection::Count);

struct LightingBudget
{
    f32 directLightingMs{0.8F};
    f32 visibilityMs{0.8F};
    f32 giMs{2.2F};
    f32 reflectionMs{1.0F};
    f32 emissiveMs{0.5F};
    f32 postProcessMs{0.8F};

    [[nodiscard]] f32 TotalMs() const noexcept;
    [[nodiscard]] f32 SectionMs(
        LightingGpuSection section) const noexcept;
};

struct LightingGpuTimings
{
    std::array<f32, kLightingGpuSectionCount>
        milliseconds{};
    std::array<bool, kLightingGpuSectionCount>
        valid{};

    [[nodiscard]] bool HasSection(
        LightingGpuSection section) const noexcept;
    [[nodiscard]] f32 SectionMs(
        LightingGpuSection section) const noexcept;
    [[nodiscard]] f32 TotalMs() const noexcept;
};

struct LightingRequestedWork
{
    u32 exactVisibilityQueries{0U};
    u32 radianceCacheUpdates{0U};
    u32 reflectionQueries{0U};
    u32 emissiveUpdates{0U};
};

struct LightingWorkPlan
{
    LightingBudget budget{};
    LightingRequestedWork requested{};

    u32 exactVisibilityQueries{0U};
    u32 radianceCacheUpdates{0U};
    u32 reflectionQueries{0U};
    u32 emissiveUpdates{0U};

    f32 visibilityScale{1.0F};
    f32 giScale{1.0F};
    f32 reflectionScale{1.0F};
    f32 emissiveScale{1.0F};

    bool hardwareRayQueryAvailable{false};
    bool preferHardwareRayQuery{false};

    [[nodiscard]] f32 TotalBudgetMs() const noexcept
    {
        return budget.TotalMs();
    }
};

struct LightingSchedulerConfig
{
    LightingBudget budget{};

    f32 minimumVisibilityScale{0.10F};
    f32 minimumGiScale{0.10F};
    f32 minimumReflectionScale{0.05F};
    f32 minimumEmissiveScale{0.10F};

    // Downward response is intentionally faster than upward recovery so a
    // transient expensive frame sheds optional work immediately but quality
    // returns gradually after pressure disappears.
    f32 overloadResponse{0.70F};
    f32 recoveryResponse{0.12F};

    // Hardware ray query is a backend preference threshold, not a budget.
    // It never raises any count by itself.
    f32 hardwareRayQueryPreferenceThreshold{0.35F};
};

class LightingScheduler
{
public:
    explicit LightingScheduler(
        LightingSchedulerConfig config = {});

    void SetConfig(
        LightingSchedulerConfig config) noexcept;

    [[nodiscard]] const LightingSchedulerConfig&
    Config() const noexcept;

    void RecordGpuTimings(
        const LightingGpuTimings& timings) noexcept;

    [[nodiscard]] LightingWorkPlan BuildPlan(
        const LightingRequestedWork& requested,
        bool hardwareRayQueryAvailable) const noexcept;

    [[nodiscard]] const LightingGpuTimings&
    SmoothedTimings() const noexcept;

private:
    [[nodiscard]] f32 ScaleFor(
        LightingGpuSection section,
        f32 minimumScale,
        f32 previousScale) const noexcept;

    LightingSchedulerConfig config_{};
    LightingGpuTimings smoothed_{};
    bool hasTimings_{false};

    f32 visibilityScale_{1.0F};
    f32 giScale_{1.0F};
    f32 reflectionScale_{1.0F};
    f32 emissiveScale_{1.0F};
};

class LightingTimestampRecorder
{
public:
    LightingTimestampRecorder(
        rhi::Device& device,
        u32 framesInFlight);

    // Call after the frame slot's fence has completed and before the command
    // list records rendering work for that slot.
    void BeginFrame(
        rhi::CommandList& commands,
        u32 frameSlot);

    void BeginSection(
        rhi::CommandList& commands,
        u32 frameSlot,
        LightingGpuSection section);

    void EndSection(
        rhi::CommandList& commands,
        u32 frameSlot,
        LightingGpuSection section);

    // Resolve only after the frame slot's GPU work is known complete.
    [[nodiscard]] std::optional<LightingGpuTimings>
    ResolveCompletedFrame(
        u32 frameSlot) const;

    [[nodiscard]] u32 FramesInFlight() const noexcept;

private:
    [[nodiscard]] static u32 QueryIndex(
        LightingGpuSection section,
        bool end) noexcept;

    rhi::Device* device_{nullptr};
    f64 timestampPeriodNanoseconds_{0.0};
    std::vector<
        std::unique_ptr<rhi::TimestampQueryPool>>
        pools_;

    struct FrameState
    {
        std::array<bool, kLightingGpuSectionCount>
            begun{};
        std::array<bool, kLightingGpuSectionCount>
            ended{};
    };

    std::vector<FrameState> frameStates_;
};
} // namespace orbit::lighting
