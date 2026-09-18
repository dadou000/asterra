#include <orbit/terrain_gpu/GpuAeolianErosionPage.hpp>

#include "M13AeolianCompute.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace orbit::terrain_gpu
{
namespace
{
constexpr u32 kThreadGroupSize = 8U;
constexpr u64 kAirborneBytesPerCell = 8U;
constexpr u64 kExchangeBytesPerCell = 40U;
constexpr u64 kAvalancheBytesPerCell = 8U;
constexpr u64 kWindForcingBytesPerCell = 16U;
constexpr u64 kMaterialSnapshotBytesPerCell = 24U;

[[nodiscard]] u64 CheckedCellCount(
    const u32 resolution)
{
    if (resolution == 0U)
    {
        throw std::invalid_argument(
            "Orbit M13 GPU aeolian solver requires non-zero resolution.");
    }

    const u64 count =
        static_cast<u64>(resolution) *
        static_cast<u64>(resolution);

    if (count >
        std::numeric_limits<u32>::max())
    {
        throw std::invalid_argument(
            "Orbit M13 GPU aeolian page exceeds 32-bit cell addressing.");
    }

    return count;
}

[[nodiscard]] std::unique_ptr<rhi::ComputePipeline>
CompilePipeline(
    rhi::Device& device,
    const shader::Compiler& compiler,
    const char* source,
    const u32 pushDwords,
    const u32 buffers,
    const u32 storageTextures = 0U)
{
    const shader::Binary binary =
        compiler.Compile({
            .source = source,
            .entryPoint = "main",
            .stage = shader::Stage::Compute,
            .debug = false
        });

    if (binary.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile an M13 aeolian compute shader.");
    }

    return device.CreateComputePipeline({
        .computeShader = {
            .data = binary.bytecode.data(),
            .size = binary.bytecode.size()
        },
        .pushConstantDwords = pushDwords,
        .shaderResourceBuffers = buffers,
        .storageTextures = storageTextures
    });
}

[[nodiscard]] std::unique_ptr<rhi::Buffer>
CreateGpuBuffer(
    rhi::Device& device,
    const u64 sizeBytes)
{
    return device.CreateBuffer({
        .sizeBytes = sizeBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::GpuOnly,
        .initialState = rhi::ResourceState::UnorderedAccess
    });
}

void RequireBufferSize(
    const rhi::Buffer& buffer,
    const u64 required,
    const char* name)
{
    if (buffer.SizeBytes() <
        required)
    {
        throw std::invalid_argument(
            std::string(
                "Orbit M13 buffer is too small: ") +
            name);
    }
}
} // namespace

bool GpuAeolianErosionConfig::IsValid() const noexcept
{
    const auto nonnegative =
        [](const f32 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0F;
        };

    const auto positive =
        [](const f32 value)
        {
            return
                std::isfinite(value) &&
                value > 0.0F;
        };

    return
        positive(timeStepSeconds) &&
        nonnegative(capacityCoefficient) &&
        nonnegative(windSpeedExponent) &&
        shadowRayCells > 0U &&
        nonnegative(shadowStrength) &&
        nonnegative(windwardExposureGain) &&
        positive(minimumExposure) &&
        positive(maximumExposure) &&
        minimumExposure <=
            maximumExposure &&
        nonnegative(pickupRatePerSecond) &&
        nonnegative(depositionRatePerSecond) &&
        std::isfinite(reptationFraction) &&
        reptationFraction >= 0.0F &&
        reptationFraction <= 1.0F &&
        nonnegative(saltationRatePerSecond) &&
        positive(referenceSaltationWindMetersPerSecond) &&
        nonnegative(maximumSandPickupDepthPerStepMeters) &&
        nonnegative(maximumSoilPickupDepthPerStepMeters) &&
        nonnegative(maximumDepositionDepthPerStepMeters) &&
        std::isfinite(sandAvalancheReposeDegrees) &&
        sandAvalancheReposeDegrees > 0.0F &&
        sandAvalancheReposeDegrees < 90.0F &&
        std::isfinite(avalancheRelaxation) &&
        avalancheRelaxation > 0.0F &&
        avalancheRelaxation <= 1.0F &&
        nonnegative(maximumAvalancheDepthPerStepMeters) &&
        nonnegative(moistureSuppressionExponent) &&
        nonnegative(bedrockAbrasionMetersPerSecondAtReferenceWind) &&
        nonnegative(maximumBedrockAbrasionDepthPerStepMeters) &&
        positive(sandDensityKgPerCubicMeter) &&
        positive(soilDensityKgPerCubicMeter);
}

GpuAeolianErosionPage::GpuAeolianErosionPage(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const u32 maxResolution,
    const u32 maxGeologyEntries)
    : maxResolution_(maxResolution),
      maxGeologyEntries_(maxGeologyEntries)
{
    const u64 maxCells =
        CheckedCellCount(
            maxResolution_);

    if (maxGeologyEntries_ == 0U)
    {
        throw std::invalid_argument(
            "Orbit M13 GPU aeolian solver requires geology capacity.");
    }

    initializePipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM13InitializeAirborneShader,
            1U,
            2U);

    exchangePipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM13ExchangeShader,
            22U,
            4U,
            4U);

    applyExchangePipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM13ApplyExchangeShader,
            2U,
            1U,
            2U);

    avalancheProposalPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM13AvalancheProposalShader,
            5U,
            1U,
            2U);

    avalancheApplyPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM13AvalancheApplyShader,
            1U,
            1U,
            1U);

    saltationPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM13SaltationShader,
            4U,
            3U);

    snapshotPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM13SnapshotMaterialShader,
            1U,
            1U,
            3U);

    airborneA_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kAirborneBytesPerCell);

    airborneB_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kAirborneBytesPerCell);

    exchange_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kExchangeBytesPerCell);

    avalanche_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kAvalancheBytesPerCell);

    geologyTable_ =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    maxGeologyEntries_) *
                sizeof(
                    terrain_geology::
                        GpuGeologicalMaterial),
            .usage =
                rhi::BufferUsage::Structured,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::ShaderResource
        });

    materialSnapshot_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kMaterialSnapshotBytesPerCell);

    materialReadback_ =
        device.CreateBuffer({
            .sizeBytes =
                maxCells *
                kMaterialSnapshotBytesPerCell,
            .usage =
                rhi::BufferUsage::Generic,
            .memory =
                rhi::MemoryUsage::HostReadback,
            .initialState =
                rhi::ResourceState::CopyDestination
        });
}

GpuAeolianErosionPage::~GpuAeolianErosionPage() = default;

void GpuAeolianErosionPage::UploadGeologyTable(
    const terrain_geology::GeologicalMaterialGpuTable& table)
{
    if (table.materials.empty() ||
        table.materials.size() !=
            table.rockTypes.size() ||
        table.materials.size() >
            maxGeologyEntries_)
    {
        throw std::invalid_argument(
            "Orbit M13 geology table is empty, inconsistent, or exceeds "
            "the fixed GPU capacity.");
    }

    const std::size_t bytes =
        table.materials.size() *
        sizeof(
            terrain_geology::
                GpuGeologicalMaterial);

    std::byte* mapped =
        geologyTable_->Map();

    std::memcpy(
        mapped,
        table.materials.data(),
        bytes);

    geologyTable_->Unmap();

    geologyUploaded_ = true;
}

void GpuAeolianErosionPage::Reset(
    rhi::CommandList& commandList,
    const u32 resolution)
{
    if (resolution == 0U ||
        resolution >
            maxResolution_)
    {
        throw std::invalid_argument(
            "Orbit M13 GPU aeolian reset resolution is invalid.");
    }

    const u32 groupCount =
        (resolution +
         kThreadGroupSize -
         1U) /
        kThreadGroupSize;

    commandList.SetComputePipeline(
        *initializePipeline_);

    commandList.SetComputeBuffer(
        0U,
        *airborneA_);

    commandList.SetComputeBuffer(
        1U,
        *airborneB_);

    const std::array<u32, 1>
        constants{
            resolution
        };

    commandList.SetComputeConstants(
        constants);

    commandList.Dispatch(
        groupCount,
        groupCount,
        1U);

    commandList.UavBarrier(
        *airborneA_);

    commandList.UavBarrier(
        *airborneB_);

    activeResolution_ =
        resolution;

    initialized_ = true;
    airborneParity_ = false;
    readbackRecorded_ = false;
}

void GpuAeolianErosionPage::DispatchSteps(
    rhi::CommandList& commandList,
    const u32 resolution,
    const f32 spacingMeters,
    const u32 stepCount,
    const GpuAeolianErosionConfig& config,
    GpuMaterialColumnResources& materialColumn,
    rhi::Buffer& windForcing)
{
    if (!config.IsValid() ||
        resolution == 0U ||
        resolution >
            maxResolution_ ||
        materialColumn.Resolution() !=
            resolution ||
        !std::isfinite(spacingMeters) ||
        spacingMeters <= 0.0F)
    {
        throw std::invalid_argument(
            "Orbit M13 GPU aeolian dispatch is invalid.");
    }

    if (!geologyUploaded_)
    {
        throw std::logic_error(
            "Orbit M13 GPU aeolian dispatch requires an uploaded M02 "
            "geology table.");
    }

    RequireBufferSize(
        windForcing,
        static_cast<u64>(
            resolution) *
            resolution *
            kWindForcingBytesPerCell,
        "windForcing");

    if (!initialized_ ||
        activeResolution_ !=
            resolution)
    {
        Reset(
            commandList,
            resolution);
    }

    if (stepCount == 0U)
    {
        return;
    }

    const u32 groupCount =
        (resolution +
         kThreadGroupSize -
         1U) /
        kThreadGroupSize;

    for (u32 step = 0U;
         step < stepCount;
         ++step)
    {
        rhi::Buffer* airborneIn =
            airborneParity_
                ? airborneB_.get()
                : airborneA_.get();

        rhi::Buffer* airborneOut =
            airborneParity_
                ? airborneA_.get()
                : airborneB_.get();

        // 1. Capacity, shadowing, pickup/deposition and reptation proposal.
        commandList.SetComputePipeline(
            *exchangePipeline_);

        commandList.SetComputeBuffer(
            0U,
            *airborneIn);

        commandList.SetComputeBuffer(
            1U,
            windForcing);

        commandList.SetComputeBuffer(
            2U,
            *geologyTable_);

        commandList.SetComputeBuffer(
            3U,
            *exchange_);

        commandList.SetComputeStorageTexture(
            0U,
            materialColumn.
                BedrockHeight());

        commandList.SetComputeStorageTexture(
            1U,
            materialColumn.
                LooseMaterials());

        commandList.SetComputeStorageTexture(
            2U,
            materialColumn.
                MoistureProcess());

        commandList.SetComputeStorageTexture(
            3U,
            materialColumn.
                GeologicalMaterial());

        std::array<u32, 22>
            exchangeConstants{};

        exchangeConstants[0] =
            resolution;

        exchangeConstants[1] =
            std::bit_cast<u32>(
                spacingMeters);

        exchangeConstants[2] =
            std::bit_cast<u32>(
                config.
                    timeStepSeconds);

        exchangeConstants[3] =
            std::bit_cast<u32>(
                config.
                    capacityCoefficient);

        exchangeConstants[4] =
            std::bit_cast<u32>(
                config.
                    windSpeedExponent);

        exchangeConstants[5] =
            config.
                shadowRayCells;

        const std::array<f32, 16>
            exchangeFloats{
                config.shadowStrength,
                config.windwardExposureGain,
                config.minimumExposure,
                config.maximumExposure,
                config.pickupRatePerSecond,
                config.depositionRatePerSecond,
                config.reptationFraction,
                config.maximumSandPickupDepthPerStepMeters,
                config.maximumSoilPickupDepthPerStepMeters,
                config.maximumDepositionDepthPerStepMeters,
                config.moistureSuppressionExponent,
                config.bedrockAbrasionMetersPerSecondAtReferenceWind,
                config.maximumBedrockAbrasionDepthPerStepMeters,
                config.referenceSaltationWindMetersPerSecond,
                config.sandDensityKgPerCubicMeter,
                config.soilDensityKgPerCubicMeter
            };

        for (u32 i = 0U;
             i <
                 exchangeFloats.size();
             ++i)
        {
            exchangeConstants[i + 6U] =
                std::bit_cast<u32>(
                    exchangeFloats[i]);
        }

        commandList.SetComputeConstants(
            exchangeConstants);

        commandList.Dispatch(
            groupCount,
            groupCount,
            1U);

        commandList.UavBarrier(
            *exchange_);

        // 2. Apply local pickup/deposition and gather reptation.
        commandList.SetComputePipeline(
            *applyExchangePipeline_);

        commandList.SetComputeBuffer(
            0U,
            *exchange_);

        commandList.SetComputeStorageTexture(
            0U,
            materialColumn.
                BedrockHeight());

        commandList.SetComputeStorageTexture(
            1U,
            materialColumn.
                LooseMaterials());

        const std::array<u32, 2>
            applyConstants{
                resolution,
                std::bit_cast<u32>(
                    config.
                        sandDensityKgPerCubicMeter)
            };

        commandList.SetComputeConstants(
            applyConstants);

        commandList.Dispatch(
            groupCount,
            groupCount,
            1U);

        commandList.UavBarrier(
            materialColumn.
                BedrockHeight());

        commandList.UavBarrier(
            materialColumn.
                LooseMaterials());

        // 3. Sand slip-face avalanche proposal.
        commandList.SetComputePipeline(
            *avalancheProposalPipeline_);

        commandList.SetComputeBuffer(
            0U,
            *avalanche_);

        commandList.SetComputeStorageTexture(
            0U,
            materialColumn.
                BedrockHeight());

        commandList.SetComputeStorageTexture(
            1U,
            materialColumn.
                LooseMaterials());

        const std::array<u32, 5>
            avalancheConstants{
                resolution,
                std::bit_cast<u32>(
                    spacingMeters),
                std::bit_cast<u32>(
                    config.
                        sandAvalancheReposeDegrees),
                std::bit_cast<u32>(
                    config.
                        avalancheRelaxation),
                std::bit_cast<u32>(
                    config.
                        maximumAvalancheDepthPerStepMeters)
            };

        commandList.SetComputeConstants(
            avalancheConstants);

        commandList.Dispatch(
            groupCount,
            groupCount,
            1U);

        commandList.UavBarrier(
            *avalanche_);

        // 4. Gather/apply sand avalanche.
        commandList.SetComputePipeline(
            *avalancheApplyPipeline_);

        commandList.SetComputeBuffer(
            0U,
            *avalanche_);

        commandList.SetComputeStorageTexture(
            0U,
            materialColumn.
                LooseMaterials());

        const std::array<u32, 1>
            avalancheApplyConstants{
                resolution
            };

        commandList.SetComputeConstants(
            avalancheApplyConstants);

        commandList.Dispatch(
            groupCount,
            groupCount,
            1U);

        commandList.UavBarrier(
            materialColumn.
                LooseMaterials());

        // 5. Gather saltation into the next airborne state.
        commandList.SetComputePipeline(
            *saltationPipeline_);

        commandList.SetComputeBuffer(
            0U,
            *exchange_);

        commandList.SetComputeBuffer(
            1U,
            windForcing);

        commandList.SetComputeBuffer(
            2U,
            *airborneOut);

        const std::array<u32, 4>
            saltationConstants{
                resolution,
                std::bit_cast<u32>(
                    config.
                        timeStepSeconds),
                std::bit_cast<u32>(
                    config.
                        saltationRatePerSecond),
                std::bit_cast<u32>(
                    config.
                        referenceSaltationWindMetersPerSecond)
            };

        commandList.SetComputeConstants(
            saltationConstants);

        commandList.Dispatch(
            groupCount,
            groupCount,
            1U);

        commandList.UavBarrier(
            *airborneOut);

        airborneParity_ =
            !airborneParity_;
    }

    readbackRecorded_ = false;
}

void GpuAeolianErosionPage::RecordMaterialReadback(
    rhi::CommandList& commandList,
    const u32 resolution,
    GpuMaterialColumnResources& materialColumn)
{
    if (!initialized_ ||
        resolution !=
            activeResolution_ ||
        materialColumn.Resolution() !=
            resolution)
    {
        throw std::invalid_argument(
            "Orbit M13 material readback does not match the active GPU page.");
    }

    const u32 groupCount =
        (resolution +
         kThreadGroupSize -
         1U) /
        kThreadGroupSize;

    commandList.SetComputePipeline(
        *snapshotPipeline_);

    commandList.SetComputeBuffer(
        0U,
        *materialSnapshot_);

    commandList.SetComputeStorageTexture(
        0U,
        materialColumn.
            BedrockHeight());

    commandList.SetComputeStorageTexture(
        1U,
        materialColumn.
            LooseMaterials());

    commandList.SetComputeStorageTexture(
        2U,
        materialColumn.
            MoistureProcess());

    const std::array<u32, 1>
        constants{
            resolution
        };

    commandList.SetComputeConstants(
        constants);

    commandList.Dispatch(
        groupCount,
        groupCount,
        1U);

    commandList.UavBarrier(
        *materialSnapshot_);

    const u64 bytes =
        static_cast<u64>(
            resolution) *
        resolution *
        kMaterialSnapshotBytesPerCell;

    commandList.Transition(
        *materialSnapshot_,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::CopySource);

    commandList.CopyBuffer(
        *materialSnapshot_,
        0U,
        *materialReadback_,
        0U,
        bytes);

    commandList.Transition(
        *materialSnapshot_,
        rhi::ResourceState::CopySource,
        rhi::ResourceState::UnorderedAccess);

    readbackRecorded_ = true;
}

void GpuAeolianErosionPage::ApplyMaterialReadbackToCpu(
    terrain_material_column::MaterialColumnPage& page)
{
    if (!readbackRecorded_ ||
        page.Resolution() !=
            activeResolution_)
    {
        throw std::logic_error(
            "Orbit M13 CPU material sync requires a completed matching "
            "RecordMaterialReadback submission.");
    }

    const std::byte* mapped =
        materialReadback_->Map();

    for (u32 y = 0U;
         y < activeResolution_;
         ++y)
    {
        for (u32 x = 0U;
             x < activeResolution_;
             ++x)
        {
            const u64 index =
                static_cast<u64>(y) *
                    activeResolution_ +
                x;

            std::array<f32, 6>
                snapshot{};

            std::memcpy(
                snapshot.data(),
                mapped +
                    index *
                        kMaterialSnapshotBytesPerCell,
                kMaterialSnapshotBytesPerCell);

            auto cell =
                page.At(
                    x,
                    y);

            cell.bedrockHeightMeters =
                snapshot[0];

            cell.regolithMeters =
                std::max(
                    snapshot[1],
                    0.0F);

            cell.soilMeters =
                std::max(
                    snapshot[2],
                    0.0F);

            cell.sandMeters =
                std::max(
                    snapshot[3],
                    0.0F);

            cell.debrisMeters =
                std::max(
                    snapshot[4],
                    0.0F);

            cell.moisture =
                std::clamp(
                    snapshot[5],
                    0.0F,
                    1.0F);

            page.SetCell(
                x,
                y,
                cell,
                false);
        }
    }

    materialReadback_->Unmap();

    readbackRecorded_ = false;
}

rhi::Buffer&
GpuAeolianErosionPage::AirborneSediment() noexcept
{
    return airborneParity_
        ? *airborneB_
        : *airborneA_;
}

rhi::Buffer&
GpuAeolianErosionPage::ExchangeProposals() noexcept
{
    return *exchange_;
}

rhi::Buffer&
GpuAeolianErosionPage::AvalancheProposals() noexcept
{
    return *avalanche_;
}
} // namespace orbit::terrain_gpu
