#pragma once

#include <orbit/weather_gpu/GpuStormSolver.hpp>

#include <cstdint>
#include <memory>
#include <string>

// Phase 7 of the storm domain plan: the nest on the GPU. A fine child GpuStormSolver (dx / ratio) inside the parent, in the same Galilean
// frame and on the same vertical grid. The parent steps on its own; the child takes `childSubsteps` steps per parent step, and after the
// forcing of each one its edge band is relaxed toward the parent (interpolated in space and between the parent's start-of-step and
// end-of-step state), before the pressure projection removes the divergence that introduces. This is NestedStormSolver on the GPU, one-way.
// The exported cloud buffer carries both levels, so the renderer draws the child's finer extinction where it exists.
namespace orbit::weather_gpu
{
struct NestedGpuConfig
{
    weather_lab::FastStormConfig parent{};
    std::uint32_t ratio{4};                      // parent dx / child dx: 2 or 4
    std::uint32_t cells{32};                     // child extent in parent cells; cells * ratio is a power of two, at most 256
    std::uint32_t originI{0xFFFFFFFFU};          // child lower-left in parent cells; default centres it
    std::uint32_t originJ{0xFFFFFFFFU};
    std::uint32_t relaxCells{12};                // width of the relaxation band, in child cells
    float relaxSeconds{10.0F};
    std::uint32_t childSubsteps{2};              // child steps per parent step (fixed, so a run is reproducible and checkpointable)
};

class NestedGpuSolver
{
public:
    NestedGpuSolver(rhi::Device& device, const shader::Compiler& compiler, const NestedGpuConfig& config);
    ~NestedGpuSolver();
    NestedGpuSolver(const NestedGpuSolver&) = delete;
    NestedGpuSolver& operator=(const NestedGpuSolver&) = delete;

    [[nodiscard]] static std::string Validate(const NestedGpuConfig& config);

    // Recompiles the kernels of both levels from `source` (see GpuStormSolver::ReloadShaders); a failure leaves both running.
    [[nodiscard]] std::string ReloadShaders(std::string_view source = {});

    // Copies the CPU parent's initial state in and fills the child from it.
    void Upload(weather_lab::FastStormSolver& cpuParent);
    // `steps` parent steps of `dt` seconds, each followed by the child's sub-steps.
    void Advance(std::uint32_t steps, float dt);

    // Both levels: the placement's `child` field is filled in here. The buffer then holds the parent's cells followed by the child's.
    void ExportCloud(StormPlacement placement);
    [[nodiscard]] rhi::Buffer& CloudBuffer();

    // Both levels in one checkpoint; restoring and stepping on continues bit for bit.
    [[nodiscard]] StormCheckpoint Save();
    void Restore(const StormCheckpoint& checkpoint);
    [[nodiscard]] std::size_t CheckpointBytes() const;

    [[nodiscard]] double Time() const;
    [[nodiscard]] double LastAdvanceMilliseconds() const;
    [[nodiscard]] std::size_t ResidentBytes() const;
    [[nodiscard]] GpuStormSolver& Parent();
    [[nodiscard]] GpuStormSolver& Child();
    // The child grid's west and south edges, metres east and north of the parent's centre.
    void ChildWestSouthEdge(float& east, float& north) const;
    [[nodiscard]] const NestedGpuConfig& Config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::weather_gpu
