#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Fence.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain_gpu/GpuFieldGenerator.hpp>
#include <orbit/world/Planet.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
// Opaque handle returned by GpuElevationQuery::Request(), used to poll
// TryGetResult() for that specific request.
struct ElevationQueryHandle
{
    u64 id{0};

    [[nodiscard]] bool IsValid() const noexcept
    {
        return id != 0;
    }
};

// Async GPU->CPU elevation readback for call sites that need a ground
// height but can tolerate a few frames of latency (camera ground-clamp,
// teleport, SLEW) -- see the GPU terrain generation plan's Milestones 3
// and 4. Replaces a direct, synchronous terrain::AnalyticTerrainSource::
// Sample() call with a request/poll pair backed by a small fixed pool of
// compute dispatches, so these call sites no longer keep a duplicate CPU
// evaluation path around just to stay synchronous.
//
// Each request runs the *full* GPU terrain stack -- not just the raw
// procedural field, but a small local neighborhood through depression
// filling and erosion too -- so ground-clamp collision reflects the same
// hydrology-shaped terrain (rivers cut in, lake beds filled) the rest of
// Milestone 4 computes, without needing a duplicate CPU hydrology
// evaluation kept around just for this. Over a filled depression (a
// lake), the returned elevation is the water surface (the conditioned
// drainage level), not the raw lake-bed elevation, so collision rests on
// the water the way it would visually.
//
// Usage, once per frame from the caller's own render loop:
//   1. Request(direction) any time a fresh sample near that direction is
//      wanted -- cheap, does no GPU work itself, just queues one.
//   2. Flush(commandList, fence, submittedFenceValue), with the SAME
//      command list about to be submitted this frame and the fence value
//      that submission will signal -- records dispatches for newly
//      queued requests, and reads back any previously-dispatched request
//      whose recorded fence value has now retired (fence.CompletedValue()
//      caught up), refreshing the LastKnownGood cache.
//   3. LastKnownGood(direction) for *something* usable this frame
//      regardless of whether a readback near that exact direction has
//      completed yet -- falls back to GlobalTerrainFields'
//      PlateElevationEstimateMeters (cheap, already-CPU, low-frequency
//      safe) before the first real GPU answer for that area arrives.
class GpuElevationQuery
{
public:
    // `fence` must be the same fence the caller signals after
    // submitting each frame's command list (e.g. Main.cpp's own
    // frameFence) -- Request() consults its CompletedValue() before
    // reusing a slot whose prior dispatch may still be in flight, and
    // Flush() records the value that submission is *about* to signal
    // against whichever slots it dispatches this frame.
    GpuElevationQuery(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        const GpuFieldGenerator& generator,
        const terrain::GlobalTerrainFields& globalFields,
        rhi::Fence& fence,
        u32 maxInFlight = 8);
    ~GpuElevationQuery();

    GpuElevationQuery(const GpuElevationQuery&) = delete;
    GpuElevationQuery& operator=(const GpuElevationQuery&) = delete;

    [[nodiscard]] ElevationQueryHandle Request(
        const math::Double3& unitDirection);

    // `submittedFenceValue` is the value the caller's Signal() call for
    // this frame's submission will use -- not yet completed, just the
    // value to check CompletedValue() against on a later Flush().
    void Flush(
        rhi::CommandList& commandList,
        u64 submittedFenceValue);

    [[nodiscard]] bool TryGetResult(
        ElevationQueryHandle handle,
        f64& outElevationMeters) const;

    [[nodiscard]] f64 LastKnownGood(
        const math::Double3& unitDirection) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::terrain_gpu
