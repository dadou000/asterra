#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain/BakedGeology.hpp>
#include <orbit/world/Planet.hpp>

#include <functional>
#include <memory>
#include <mutex>

namespace orbit::terrain_gpu
{
struct GpuGeologyCompileProduct
{
    std::shared_ptr<const terrain::BakedGeologyRasters> rasters;
    u64 samples{0U};
    u64 eventRecords{0U};
    u64 tilesUpdated{0U};
    f64 samplesPerSecond{0.0};
    f64 eventRecordsPerSecond{0.0};
    u64 transferBytes{0U};
    u64 dispatches{0U};
    f64 fenceWaitMilliseconds{0.0};
    f64 gpuQueueMilliseconds{0.0};
    bool gpuTimestampAvailable{false};
};

// Compiles tile-local, chronologically ordered impact/resurfacing batches and
// stress-generated fracture segments on the device. Event hierarchy queries
// and canonical event resolution remain owned by terrain_impacts; only the
// per-texel evaluation moves to compute. The returned raster is built and
// validated before the bake service can activate it.
class GpuGeologyCompiler
{
public:
    GpuGeologyCompiler(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler);
    ~GpuGeologyCompiler();

    GpuGeologyCompiler(const GpuGeologyCompiler&) = delete;
    GpuGeologyCompiler& operator=(const GpuGeologyCompiler&) = delete;

    [[nodiscard]] GpuGeologyCompileProduct Compile(
        const world::PlanetDefinition& planet,
        const terrain::AnalyticTerrainDesc& desc,
        const terrain::AnalyticTerrainDesc& previousDesc,
        std::shared_ptr<const terrain::BakedGeologyRasters> previous,
        u32 resolution,
        u64 recipeHash,
        const std::function<bool()>& isCancelled,
        const std::function<void(u64, u64)>& reportProgress) const;

private:
    rhi::Device& device_;
    std::unique_ptr<rhi::ComputePipeline> pipeline_;
    mutable std::mutex compileMutex_;
};
} // namespace orbit::terrain_gpu
