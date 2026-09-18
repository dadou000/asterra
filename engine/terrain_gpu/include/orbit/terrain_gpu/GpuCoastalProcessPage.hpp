#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_gpu/GpuMaterialColumnResources.hpp>
#include <orbit/terrain_gpu/GpuSedimentExchangeResources.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/terrain_water/CoastalProcess.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
struct GpuCoastalProcessConfig
{
    f32 timeStepSeconds{0.02F};
    f32 maximumExpectedWaterDepthMeters{20.0F};

    f32 gravityMetersPerSecondSquared{9.81F};
    f32 cflNumber{0.42F};

    f32 seaLevelMeters{0.0F};
    f32 wetThresholdMeters{0.02F};
    f32 dryThresholdMeters{0.005F};

    f32 manningRoughness{0.025F};
    f32 maximumVelocityMetersPerSecond{25.0F};

    f32 waveAmplitudeMeters{0.35F};
    f32 wavePeriodSeconds{8.0F};
    f32 wavePhaseRadians{0.0F};
    f32 waveDirectionX{1.0F};
    f32 waveDirectionY{0.0F};

    f32 activeDepthMeters{12.0F};
    f32 referenceEnergySquareMetersPerSecondSquared{2.0F};
    f32 erosionMetersPerSecondAtReference{0.003F};
    f32 maximumErosionDepthPerStepMeters{0.08F};

    f32 sandBedloadFraction{0.85F};
    f32 finesBedloadFraction{0.10F};
    f32 coarseDebrisBedloadFraction{1.0F};

    f32 transportRate{1.0F};
    f32 maximumTransportFractionPerStep{0.75F};

    f32 depositionVelocityThresholdMetersPerSecond{0.55F};
    f32 depositionRatePerSecond{0.70F};
    f32 shorelineDepositionMultiplier{1.75F};

    f32 regolithDensityKgPerCubicMeter{1'650.0F};
    f32 soilDensityKgPerCubicMeter{1'300.0F};
    f32 sandDensityKgPerCubicMeter{1'600.0F};
    f32 debrisDensityKgPerCubicMeter{1'850.0F};

    f32 coastalBedrockSandFraction{0.60F};

    [[nodiscard]] bool IsValid() const noexcept;
};

// Persistent M17 GPU coastal process.
//
// Per-cell resident state:
//   water depth      f32
//   momentum         float2
//   wet mask         u32
//   shoreline mask   u32
//
// Process scratch:
//   one ping-pong water/momentum pair
//   two typed M14 float4 transport scratch buffers
//   explicit four-edge typed sediment export buffers
//
// M08 and M14 remain the physical/material authorities. This object mutates
// their GPU mirrors and offers explicit material/water readback after the
// submission fence completes.
class GpuCoastalProcessPage
{
public:
    GpuCoastalProcessPage(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        u32 maxResolution,
        u32 maxGeologyEntries = 4'096U);
    ~GpuCoastalProcessPage();

    GpuCoastalProcessPage(
        const GpuCoastalProcessPage&) = delete;
    GpuCoastalProcessPage& operator=(
        const GpuCoastalProcessPage&) = delete;

    void UploadGeologyTable(
        const terrain_geology::GeologicalMaterialGpuTable& table);

    // Empty CPU boundary state is encoded as four closed walls.
    void UploadBoundary(
        u32 resolution,
        const terrain_water::CoastalBoundaryState& boundary);

    // Optional per-cell M04 protection. Null uses an internally-owned all-zero
    // field. The buffer is N*N f32 ShaderResource data.
    void DispatchSteps(
        rhi::CommandList& commandList,
        u32 resolution,
        f32 spacingMeters,
        u32 stepCount,
        const GpuCoastalProcessConfig& config,
        GpuMaterialColumnResources& materialColumn,
        GpuSedimentExchangeResources& sediment,
        rhi::Buffer* protection = nullptr);

    void Reset(
        rhi::CommandList& commandList,
        u32 resolution,
        const GpuCoastalProcessConfig& config,
        GpuMaterialColumnResources& materialColumn);

    // Records water/momentum/masks and M08 material into persistent host
    // readback buffers. Apply*ReadbackToCpu must be called only after the
    // submission fence for this command list has completed.
    void RecordReadback(
        rhi::CommandList& commandList,
        u32 resolution,
        GpuMaterialColumnResources& materialColumn);

    void ApplyWaterReadbackToCpu(
        terrain_water::CoastalWaterPage& page,
        f64 spacingMeters);

    void ApplyMaterialReadbackToCpu(
        terrain_material_column::MaterialColumnPage& page);

    [[nodiscard]] rhi::Buffer& WaterDepth() noexcept;
    [[nodiscard]] rhi::Buffer& Momentum() noexcept;
    [[nodiscard]] rhi::Buffer& WetMask() noexcept;
    [[nodiscard]] rhi::Buffer& ShorelineMask() noexcept;

    // Four sides packed north/east/south/west, N float4 records each. Values
    // are typed M14 kg/m2 (sand/fines/coarse/reserved).
    [[nodiscard]] rhi::Buffer& BoundaryWaterborneExport() noexcept;
    [[nodiscard]] rhi::Buffer& BoundarySurfaceExport() noexcept;

private:
    u32 maxResolution_{0};
    u32 maxGeologyEntries_{0};
    u32 activeResolution_{0};
    u32 boundaryResolution_{0};

    bool initialized_{false};
    bool boundaryUploaded_{false};
    bool geologyUploaded_{false};
    bool stateParity_{false};
    bool readbackRecorded_{false};

    f32 elapsedSeconds_{0.0F};
    GpuCoastalProcessConfig activeConfig_{};

    std::unique_ptr<rhi::ComputePipeline> initializePipeline_;
    std::unique_ptr<rhi::ComputePipeline> advancePipeline_;
    std::unique_ptr<rhi::ComputePipeline> shorelinePipeline_;
    std::unique_ptr<rhi::ComputePipeline> sedimentExchangePipeline_;
    std::unique_ptr<rhi::ComputePipeline> sedimentTransportPipeline_;
    std::unique_ptr<rhi::ComputePipeline> sedimentDepositPipeline_;
    std::unique_ptr<rhi::ComputePipeline> snapshotPipeline_;

    std::unique_ptr<rhi::Buffer> waterA_;
    std::unique_ptr<rhi::Buffer> waterB_;
    std::unique_ptr<rhi::Buffer> momentumA_;
    std::unique_ptr<rhi::Buffer> momentumB_;

    std::unique_ptr<rhi::Buffer> wet_;
    std::unique_ptr<rhi::Buffer> shoreline_;

    std::unique_ptr<rhi::Buffer> waterborneScratch_;
    std::unique_ptr<rhi::Buffer> surfaceScratch_;

    std::unique_ptr<rhi::Buffer> boundaryWaterborneExport_;
    std::unique_ptr<rhi::Buffer> boundarySurfaceExport_;

    std::unique_ptr<rhi::Buffer> boundary_;
    std::unique_ptr<rhi::Buffer> zeroProtection_;
    std::unique_ptr<rhi::Buffer> geologyTable_;

    std::unique_ptr<rhi::Buffer> waterReadback_;

    std::unique_ptr<rhi::Buffer> materialSnapshot_;
    std::unique_ptr<rhi::Buffer> materialReadback_;
};
} // namespace orbit::terrain_gpu
