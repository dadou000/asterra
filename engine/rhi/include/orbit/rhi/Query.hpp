#pragma once

#include <orbit/core/Types.hpp>

namespace orbit::rhi
{
// A pool of GPU timestamp slots for per-pass frame timing (see
// CommandList::WriteTimestamp). Answers "where does frame time actually
// go on the GPU" without needing RenderDoc or a separate profiler open.
class TimestampQueryPool
{
public:
    virtual ~TimestampQueryPool() = default;

    TimestampQueryPool(const TimestampQueryPool&) = delete;
    TimestampQueryPool& operator=(const TimestampQueryPool&) = delete;

    [[nodiscard]] virtual u32 Count() const noexcept = 0;

    // Reads back raw GPU timestamp ticks (convert to nanoseconds via
    // Device::TimestampPeriodNanoseconds) for queries [first, first +
    // count). This does not itself wait for the GPU: the caller must
    // already know the work that wrote them has completed -- e.g. via
    // the same per-frame fence wait already used before reusing that
    // frame's allocator. Returns false (leaving outTicks untouched) if
    // any query in the range hasn't been written yet.
    [[nodiscard]] virtual bool TryGetResults(
        u32 first,
        u32 count,
        u64* outTicks) const = 0;

protected:
    TimestampQueryPool() = default;
};
} // namespace orbit::rhi
