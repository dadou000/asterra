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
struct GpuAeolianErosionConfig
{
    f32 timeStepSeconds{0.20F};

    f32 capacityCoefficient{0.030F};
    f32 windSpeedExponent{2.0F};

    u32 shadowRayCells{4U};
    f32 shadowStrength{8.0F};
    f32 windwardExposureGain{0.50F};
    f32 minimumExposure{0.05F};
    f32 maximumExposure{1.75F};

    f32 pickupRatePerSecond{1.25F};
    f32 depositionRatePerSecond{1.50F};
    f32 reptationFraction{0.20F};

    f32 saltationRatePerSecond{2.0F};
    f32 referenceSaltationWindMetersPerSecond{10.0F};

    f32 maximumSandPickupDepthPerStepMeters{0.05F};
    f32 maximumSoilPickupDepthPerStepMeters{0.02F};
    f32 maximumDepositionDepthPerStepMeters{0.08F};

    f32 sandAvalancheReposeDegrees{33.0F};
    f32 avalancheRelaxation{0.50F};
    f32 maximumAvalancheDepthPerStepMeters{0.08F};

    f32 moistureSuppressionExponent{2.0F};

    f32 bedrockAbrasionMetersPerSecondAtReferenceWind{2.0e-6F};
    f32 maximumBedrockAbrasionDepthPerStepMeters{2.0e-4F};

    f32 sandDensityKgPerCubicMeter{1'600.0F};
    f32 soilDensityKgPerCubicMeter{1'300.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

// Wind-forcing buffer layout: one float4 per cell:
//   x = east wind m/s
//   y = north wind m/s
//   z = surface/vegetation/protection resistance [0,1]
//   w = reserved
//
// Airborne state is float2 kg/m2:
//   x = sand
//   y = soil/fines
//
// M08 remains CPU terrain authority. RecordMaterialReadback() and
// ApplyMaterialReadbackToCpu() synchronize the physical result after the
// recorded submission fence completes.
class GpuAeolianErosionPage
{
public:
    GpuAeolianErosionPage(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        u32 maxResolution,
        u32 maxGeologyEntries = 4'096U);
    ~GpuAeolianErosionPage();

    GpuAeolianErosionPage(
        const GpuAeolianErosionPage&) = delete;
    GpuAeolianErosionPage& operator=(
        const GpuAeolianErosionPage&) = delete;

    void UploadGeologyTable(
        const terrain_geology::GeologicalMaterialGpuTable& table);

    void Reset(
        rhi::CommandList& commandList,
        u32 resolution);

    void DispatchSteps(
        rhi::CommandList& commandList,
        u32 resolution,
        f32 spacingMeters,
        u32 stepCount,
        const GpuAeolianErosionConfig& config,
        GpuMaterialColumnResources& materialColumn,
        rhi::Buffer& windForcing);

    void RecordMaterialReadback(
        rhi::CommandList& commandList,
        u32 resolution,
        GpuMaterialColumnResources& materialColumn);

    void ApplyMaterialReadbackToCpu(
        terrain_material_column::MaterialColumnPage& page);

    [[nodiscard]] rhi::Buffer& AirborneSediment() noexcept;
    [[nodiscard]] rhi::Buffer& ExchangeProposals() noexcept;
    [[nodiscard]] rhi::Buffer& AvalancheProposals() noexcept;

private:
    u32 maxResolution_{0};
    u32 maxGeologyEntries_{0};
    u32 activeResolution_{0};

    bool initialized_{false};
    bool geologyUploaded_{false};
    bool airborneParity_{false};
    bool readbackRecorded_{false};

    std::unique_ptr<rhi::ComputePipeline> initializePipeline_;
    std::unique_ptr<rhi::ComputePipeline> exchangePipeline_;
    std::unique_ptr<rhi::ComputePipeline> applyExchangePipeline_;
    std::unique_ptr<rhi::ComputePipeline> avalancheProposalPipeline_;
    std::unique_ptr<rhi::ComputePipeline> avalancheApplyPipeline_;
    std::unique_ptr<rhi::ComputePipeline> saltationPipeline_;
    std::unique_ptr<rhi::ComputePipeline> snapshotPipeline_;

    std::unique_ptr<rhi::Buffer> airborneA_;
    std::unique_ptr<rhi::Buffer> airborneB_;
    std::unique_ptr<rhi::Buffer> exchange_;
    std::unique_ptr<rhi::Buffer> avalanche_;

    std::unique_ptr<rhi::Buffer> geologyTable_;

    std::unique_ptr<rhi::Buffer> materialSnapshot_;
    std::unique_ptr<rhi::Buffer> materialReadback_;
};
} // namespace orbit::terrain_gpu
