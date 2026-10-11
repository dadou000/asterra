#pragma once

#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/weather_lab/FastStormSolver.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Phase 4 of the storm domain plan (docs: /world/weather-lab/global-system-plan): the fast core on the GPU.
//
// GpuStormSolver is one level of weather_lab::FastStormSolver as Vulkan compute: the same stages in the same order with the same
// arithmetic, in float32 (the CPU reference projects in double). All state lives in one storage buffer in two ping-pong copies, so the
// whole step records into one command list and a single UAV barrier orders any two stages.
//
// Hot iteration: the kernels are one HLSL file (shaders/storm.hlsl). ReloadShaders recompiles every entry point and swaps the
// pipelines only when all of them compile, so a failed edit leaves the running generation untouched.
namespace orbit::weather_gpu
{
// Where the simulated domain sits on the planet, for the cloud renderer (see celestial_clouds CloudRenderer::SetStormBuffer).
struct StormPlacement
{
    bool active{true};
    std::array<double, 3> centre{};   // centre of the domain on the ground, planet-centred, body-fixed (m)
    std::array<float, 3> east{};      // unit tangent vectors at the centre; x of the grid runs east, y north
    std::array<float, 3> north{};
    float planetRadius{6371000.0F};
    float fadeMeters{0.0F};           // procedural-to-simulated blend at the edge; 0 = the lateral sponge width (or 8 cells)
    float intensity{1.0F};            // 1 = the simulated clouds fully replace the procedural ones inside the domain, 0 = not at all
};

// The complete simulation state at one storm time: restoring it and stepping on gives exactly the result of never having stopped.
struct StormCheckpoint
{
    double time{0.0};
    std::vector<float> data;
};

class GpuStormSolver
{
public:
    GpuStormSolver(rhi::Device& device, const shader::Compiler& compiler, const weather_lab::FastStormConfig& config);
    ~GpuStormSolver();
    GpuStormSolver(const GpuStormSolver&) = delete;
    GpuStormSolver& operator=(const GpuStormSolver&) = delete;

    // Empty when the configuration can run on the GPU (power-of-two nx and ny of at most 256, nz at most 120).
    [[nodiscard]] static std::string Validate(const weather_lab::FastStormConfig& config);

    // Recompiles the kernels from `source` (empty: the embedded copy, or the file named by ORBIT_WEATHER_GPU_SHADER when set).
    // Returns the compile error, or an empty string; on error the previous pipelines stay in use.
    [[nodiscard]] std::string ReloadShaders(std::string_view source = {});

    // Copies the CPU solver's state in (the CPU builds the initial bubble) or out.
    void Upload(weather_lab::FastStormSolver& cpu);
    void Download(weather_lab::FastStormSolver& cpu);

    // Runs `steps` steps of length `dt` (fixed: the adaptive Courant limit needs a readback per step) in one submission and waits.
    void Advance(std::uint32_t steps, float dt);

    // Reads the state back (one GPU round trip) / puts a saved state in. A restored solver continues bit for bit like the original:
    // the GPU kernels use no atomics and no unordered reductions.
    [[nodiscard]] StormCheckpoint Save();
    void Restore(const StormCheckpoint& checkpoint);
    [[nodiscard]] std::size_t CheckpointBytes() const;

    // Writes the placement header and the cloud extinction (1/m) of the current state into CloudBuffer(). The buffer is left in
    // ResourceState::ShaderResource, ready for CloudRenderer::SetStormBuffer; the call waits for the GPU.
    void ExportCloud(const StormPlacement& placement);
    [[nodiscard]] rhi::Buffer& CloudBuffer();

    [[nodiscard]] double Time() const;
    // Wall milliseconds of the last Advance (record, submit, wait).
    [[nodiscard]] double LastAdvanceMilliseconds() const;
    [[nodiscard]] std::size_t ResidentBytes() const;
    [[nodiscard]] const weather_lab::FastStormConfig& Config() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The kernels compiled into the library.
[[nodiscard]] std::string_view EmbeddedStormShader();
} // namespace orbit::weather_gpu
