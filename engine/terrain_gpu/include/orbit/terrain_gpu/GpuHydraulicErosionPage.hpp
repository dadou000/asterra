#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_gpu/GpuMaterialColumnResources.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <memory>

namespace orbit::terrain_gpu
{
struct GpuHydraulicErosionConfig
{
    f32 timeStepSeconds{0.25F};
    f32 uniformRainfallMetersPerSecond{0.0002F};

    f32 gravityMetersPerSecondSquared{9.81F};
    f32 pipeCrossSectionSquareMeters{1.0F};

    f32 sedimentCapacityCoefficient{450.0F};
    f32 maximumSedimentConcentrationKgPerCubicMeter{1'600.0F};

    f32 erosionRatePerSecond{0.35F};
    f32 depositionRatePerSecond{0.55F};

    f32 maximumErosionDepthPerStepMeters{0.25F};
    f32 maximumDepositionDepthPerStepMeters{0.25F};

    f32 regolithMobility{0.55F};
    f32 soilMobility{0.90F};
    f32 sandMobility{1.00F};
    f32 debrisMobility{0.35F};

    f32 regolithDensityKgPerCubicMeter{1'650.0F};
    f32 soilDensityKgPerCubicMeter{1'300.0F};
    f32 sandDensityKgPerCubicMeter{1'600.0F};
    f32 debrisDensityKgPerCubicMeter{1'850.0F};

    f32 infiltrationMetersPerSecond{0.00005F};
    f32 moistureCapacityDepthMeters{0.20F};
    f32 evaporationRatePerSecond{0.015F};

    [[nodiscard]] bool IsValid() const noexcept;
};

// Stateful M11 GPU page solver.
//
// Internal state:
//   water depth                  N*N f32
//   four virtual-pipe fluxes     N*N float4
//   horizontal velocity          N*N float2
//   suspended sediment           N*N f32 kg/m2
//
// The M08 GPU material page is mutated directly. CPU MaterialColumnPage remains
// authority: RecordMaterialReadback() + ApplyMaterialReadbackToCpu() synchronize
// the final GPU bedrock/loose/moisture state after the caller has submitted and
// waited for the recorded command list.
class GpuHydraulicErosionPage
{
public:
    GpuHydraulicErosionPage(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        u32 maxResolution,
        u32 maxGeologyEntries = 4'096U);
    ~GpuHydraulicErosionPage();

    GpuHydraulicErosionPage(
        const GpuHydraulicErosionPage&) = delete;
    GpuHydraulicErosionPage& operator=(
        const GpuHydraulicErosionPage&) = delete;

    // Must use the same table/order that was passed to PackGpuPage().
    void UploadGeologyTable(
        const terrain_geology::GeologicalMaterialGpuTable& table);

    void Reset(
        rhi::CommandList& commandList,
        u32 resolution,
        f32 initialWaterDepthMeters = 0.0F,
        f32 initialSedimentKgPerSquareMeter = 0.0F);

    // rainfallRateMetersPerSecond is optional N*N f32 ShaderResource data.
    // Null uses an internally-owned all-zero field in addition to uniform rain.
    void DispatchSteps(
        rhi::CommandList& commandList,
        u32 resolution,
        f32 spacingMeters,
        u32 stepCount,
        const GpuHydraulicErosionConfig& config,
        GpuMaterialColumnResources& materialColumn,
        rhi::Buffer* rainfallRateMetersPerSecond = nullptr);

    // Records a GPU material snapshot and copy into the persistent HostReadback
    // buffer. Call ApplyMaterialReadbackToCpu only after the submission fence
    // for this command list has completed.
    void RecordMaterialReadback(
        rhi::CommandList& commandList,
        u32 resolution,
        GpuMaterialColumnResources& materialColumn);

    void ApplyMaterialReadbackToCpu(
        terrain_material_column::MaterialColumnPage& page);

    [[nodiscard]] rhi::Buffer& WaterDepth() noexcept;
    [[nodiscard]] rhi::Buffer& Velocity() noexcept;
    [[nodiscard]] rhi::Buffer& SuspendedSediment() noexcept;
    [[nodiscard]] rhi::Buffer& CurrentFlux() noexcept;

private:
    u32 maxResolution_{0};
    u32 maxGeologyEntries_{0};
    u32 activeResolution_{0};

    bool initialized_{false};
    bool geologyUploaded_{false};
    bool fluxParity_{false};
    bool sedimentParity_{false};
    bool readbackRecorded_{false};

    std::unique_ptr<rhi::ComputePipeline> initializePipeline_;
    std::unique_ptr<rhi::ComputePipeline> rainFluxPipeline_;
    std::unique_ptr<rhi::ComputePipeline> waterVelocityPipeline_;
    std::unique_ptr<rhi::ComputePipeline> erodeDepositPipeline_;
    std::unique_ptr<rhi::ComputePipeline> sedimentTransportPipeline_;
    std::unique_ptr<rhi::ComputePipeline> infiltrationPipeline_;
    std::unique_ptr<rhi::ComputePipeline> snapshotPipeline_;

    std::unique_ptr<rhi::Buffer> water_;
    std::unique_ptr<rhi::Buffer> waterScratch_;
    std::unique_ptr<rhi::Buffer> fluxA_;
    std::unique_ptr<rhi::Buffer> fluxB_;
    std::unique_ptr<rhi::Buffer> velocity_;
    std::unique_ptr<rhi::Buffer> sedimentA_;
    std::unique_ptr<rhi::Buffer> sedimentB_;
    std::unique_ptr<rhi::Buffer> sedimentLocal_;

    std::unique_ptr<rhi::Buffer> materialSnapshot_;
    std::unique_ptr<rhi::Buffer> materialReadback_;

    std::unique_ptr<rhi::Buffer> zeroRainfall_;
    std::unique_ptr<rhi::Buffer> geologyTable_;
};
} // namespace orbit::terrain_gpu
