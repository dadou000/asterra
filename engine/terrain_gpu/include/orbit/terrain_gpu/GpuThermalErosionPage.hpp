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
struct GpuThermalErosionConfig
{
    f32 sandReposeDegrees{33.0F};
    f32 debrisReposeDegrees{40.0F};
    f32 regolithReposeDegrees{37.0F};
    f32 soilReposeDegrees{46.0F};

    f32 minimumBedrockFailureDegrees{58.0F};
    f32 bedrockFailureAngleRangeDegrees{24.0F};
    f32 bedrockFractureRate{0.20F};

    f32 relaxation{0.50F};
    f32 maximumTransferDepthPerIterationMeters{0.50F};

    f32 regolithDensityKgPerCubicMeter{1'650.0F};
    f32 soilDensityKgPerCubicMeter{1'300.0F};
    f32 sandDensityKgPerCubicMeter{1'600.0F};
    f32 debrisDensityKgPerCubicMeter{1'850.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

// M12 GPU local thermal/gravity relaxation. Each pass first computes one
// steepest-neighbor proposal per source cell, then a gather pass applies
// outgoing/incoming material simultaneously. This avoids float atomics and
// iteration-order dependence.
//
// M08 remains CPU authority. RecordMaterialReadback() and
// ApplyMaterialReadbackToCpu() synchronize the final mutated physical column
// after the command-list fence has completed.
class GpuThermalErosionPage
{
public:
    GpuThermalErosionPage(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        u32 maxResolution,
        u32 maxGeologyEntries = 4'096U);
    ~GpuThermalErosionPage();

    GpuThermalErosionPage(
        const GpuThermalErosionPage&) = delete;
    GpuThermalErosionPage& operator=(
        const GpuThermalErosionPage&) = delete;

    // Must use the same table ordering used by PackGpuPage().
    void UploadGeologyTable(
        const terrain_geology::GeologicalMaterialGpuTable& table);

    // protection is optional N*N f32 ShaderResource [0,1]. Null selects the
    // internally-owned all-zero field.
    void DispatchSteps(
        rhi::CommandList& commandList,
        u32 resolution,
        f32 spacingMeters,
        u32 stepCount,
        const GpuThermalErosionConfig& config,
        GpuMaterialColumnResources& materialColumn,
        rhi::Buffer* protection = nullptr);

    void RecordMaterialReadback(
        rhi::CommandList& commandList,
        u32 resolution,
        GpuMaterialColumnResources& materialColumn);

    void ApplyMaterialReadbackToCpu(
        terrain_material_column::MaterialColumnPage& page);

    [[nodiscard]] rhi::Buffer& TransferProposals() noexcept;

private:
    u32 maxResolution_{0};
    u32 maxGeologyEntries_{0};
    u32 activeResolution_{0};

    bool geologyUploaded_{false};
    bool readbackRecorded_{false};

    std::unique_ptr<rhi::ComputePipeline> computeTransferPipeline_;
    std::unique_ptr<rhi::ComputePipeline> applyTransferPipeline_;
    std::unique_ptr<rhi::ComputePipeline> snapshotPipeline_;

    std::unique_ptr<rhi::Buffer> transfers_;
    std::unique_ptr<rhi::Buffer> zeroProtection_;
    std::unique_ptr<rhi::Buffer> geologyTable_;

    std::unique_ptr<rhi::Buffer> materialSnapshot_;
    std::unique_ptr<rhi::Buffer> materialReadback_;
};
} // namespace orbit::terrain_gpu
