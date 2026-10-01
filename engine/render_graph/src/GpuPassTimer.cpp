#include <orbit/render_graph/GpuPassTimer.hpp>

#include <orbit/profiler/Profiler.hpp>

#include <algorithm>

namespace orbit::render_graph
{
namespace
{
// Passes shorter than this are kept in LastFrame() but not put on the timeline,
// which would otherwise fill the lane's ring buffer with noise.
constexpr f64 kLaneMinimumMs = 0.05;
} // namespace

GpuPassTimer::GpuPassTimer(
    rhi::Device& device,
    const u32 framesInFlight,
    const u32 maxTimestampsPerFrame)
    : device_(&device)
    , periodNanoseconds_(device.TimestampPeriodNanoseconds())
    , maxMarks_(std::max(maxTimestampsPerFrame, 8U))
{
    slots_.resize(std::max(framesInFlight, 1U));
    for (Slot& slot : slots_)
    {
        slot.pool = device.CreateTimestampQueryPool(maxMarks_);
        slot.names.reserve(maxMarks_);
    }
}

void GpuPassTimer::BeginFrame(rhi::CommandList& commands, const u32 slot)
{
    active_ = nullptr;
    if (!profiler::Enabled() || slot >= slots_.size() || periodNanoseconds_ <= 0.0)
    {
        return;
    }

    Slot& target = slots_[slot];
    commands.ResetTimestampQueryPool(*target.pool, 0U, maxMarks_);
    commands.WriteTimestamp(*target.pool, 0U);
    target.names.clear();
    target.marks = 1U;
    target.recorded = true;
    active_ = &target;
}

void GpuPassTimer::Mark(rhi::CommandList& commands, const char* name)
{
    if (active_ == nullptr || active_->marks >= maxMarks_)
    {
        return;
    }
    commands.WriteTimestamp(*active_->pool, active_->marks);
    active_->names.push_back(name);
    ++active_->marks;
}

void GpuPassTimer::Resolve(const u32 slot)
{
    if (slot >= slots_.size())
    {
        return;
    }
    Slot& source = slots_[slot];
    if (!source.recorded)
    {
        return;
    }
    source.recorded = false;
    if (source.marks < 2U)
    {
        return;
    }

    std::vector<u64> ticks(source.marks);
    // False when the commands never ran (an abandoned frame): nothing to report.
    if (!source.pool->TryGetResults(0U, source.marks, ticks.data()))
    {
        return;
    }

    // Pair the GPU clock with the profiler clock (QPC) now, then express every
    // timestamp relative to that pair.
    u64 gpuNow = 0U;
    u64 cpuNow = 0U;
    calibrated_ = device_->CalibrateGpuClock(&gpuNow, &cpuNow);
    if (!calibrated_)
    {
        gpuNow = ticks.back();
        cpuNow = profiler::NowTicks();
    }
    const f64 cpuTicksPerGpuTick =
        periodNanoseconds_ * 1.0e-9 * profiler::TicksPerMillisecond() * 1000.0;
    const auto toCpu = [&](const u64 gpuTicks) -> u64
    {
        const f64 offset = gpuTicks <= gpuNow
            ? -static_cast<f64>(gpuNow - gpuTicks) * cpuTicksPerGpuTick
            : static_cast<f64>(gpuTicks - gpuNow) * cpuTicksPerGpuTick;
        const f64 cpu = static_cast<f64>(cpuNow) + offset;
        return cpu > 0.0 ? static_cast<u64>(cpu) : 0U;
    };
    const auto toMilliseconds = [&](const u64 from, const u64 to)
    {
        return to > from
            ? static_cast<f64>(to - from) * periodNanoseconds_ * 1.0e-6
            : 0.0;
    };

    last_.valid = true;
    last_.passes.clear();
    last_.totalMs = toMilliseconds(ticks.front(), ticks.back());

    for (u32 i = 1U; i < source.marks; ++i)
    {
        const f64 milliseconds = toMilliseconds(ticks[i - 1U], ticks[i]);
        last_.passes.push_back({source.names[i - 1U], milliseconds});
        if (milliseconds >= kLaneMinimumMs)
        {
            profiler::RecordLaneSpan(
                "GPU passes",
                source.names[i - 1U],
                toCpu(ticks[i - 1U]),
                toCpu(ticks[i]));
        }
    }
    profiler::RecordLaneSpan(
        "GPU frames",
        "gpu frame",
        toCpu(ticks.front()),
        toCpu(ticks.back()));
}
} // namespace orbit::render_graph
