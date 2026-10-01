#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Query.hpp>

#include <memory>
#include <vector>

namespace orbit::render_graph
{
struct GpuPassTiming
{
    // A string literal or interned string (the render pass name).
    const char* name{""};
    f64 milliseconds{0.0};
};

struct GpuFrameTimings
{
    bool valid{false};
    // First to last timestamp of the frame's command list.
    f64 totalMs{0.0};
    std::vector<GpuPassTiming> passes;
};

// Per-render-pass GPU timing for the CPU micro-profiler (docs/ORBIT_PROFILER.md).
//
// A timestamp is written after every pass's recording callback. They are
// BOTTOM_OF_PIPE, so each one fires once everything before it has finished: the
// gap between two neighbours is the GPU time of the pass between them, including
// time stuck behind earlier work. Results are read after the frame slot's fence
// has completed, converted to the CPU profiler's clock with the device's
// calibrated timestamps, and published on the "GPU passes" and "GPU frames"
// lanes, so a pass that holds the GPU for seconds shows up beside the CPU threads
// that were waiting on it.
//
// Call order per frame slot: Resolve(slot) once its fence has completed, then
// BeginFrame(commands, slot) right after the command list is reset and before any
// render target is set, then RenderGraph::Execute(commands, &timer).
class GpuPassTimer
{
public:
    GpuPassTimer(
        rhi::Device& device,
        u32 framesInFlight,
        u32 maxTimestampsPerFrame = 384U);

    void BeginFrame(rhi::CommandList& commands, u32 slot);

    // Closes the region that started at the previous mark and names it.
    void Mark(rhi::CommandList& commands, const char* name);

    void Resolve(u32 slot);

    [[nodiscard]] const GpuFrameTimings& LastFrame() const noexcept
    {
        return last_;
    }

    // True when GPU spans are placed on the CPU timeline exactly; otherwise they
    // are anchored to the moment the frame was resolved.
    [[nodiscard]] bool Calibrated() const noexcept { return calibrated_; }

private:
    struct Slot
    {
        std::unique_ptr<rhi::TimestampQueryPool> pool;
        std::vector<const char*> names;
        u32 marks{0U};
        bool recorded{false};
    };

    rhi::Device* device_{nullptr};
    f64 periodNanoseconds_{0.0};
    u32 maxMarks_{0U};
    std::vector<Slot> slots_;
    Slot* active_{nullptr};
    GpuFrameTimings last_;
    bool calibrated_{false};
};
} // namespace orbit::render_graph
