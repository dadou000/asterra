#include <orbit/terrain_gpu/GpuHydraulicErosionPage.hpp>

#include "M11HydraulicCompute.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace orbit::terrain_gpu
{
namespace
{
constexpr u32 kThreadGroupSize = 8U;
constexpr u64 kFluxBytesPerCell = 16U;
constexpr u64 kVelocityBytesPerCell = 8U;
constexpr u64 kScalarBytesPerCell = 4U;
constexpr u64 kMaterialSnapshotBytesPerCell = 24U;

[[nodiscard]] u64 CheckedCellCount(
    const u32 resolution)
{
    if (resolution == 0U)
    {
        throw std::invalid_argument(
            "Orbit M11 GPU hydraulic solver requires non-zero resolution.");
    }

    const u64 count =
        static_cast<u64>(resolution) *
        static_cast<u64>(resolution);

    if (count >
        std::numeric_limits<u32>::max())
    {
        throw std::invalid_argument(
            "Orbit M11 GPU hydraulic page exceeds 32-bit cell addressing.");
    }

    return count;
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
            "Orbit failed to compile an M11 hydraulic compute shader.");
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

void RequireBufferSize(
    const rhi::Buffer& buffer,
    const u64 required,
    const char* name)
{
    if (buffer.SizeBytes() < required)
    {
        throw std::invalid_argument(
            std::string(
                "Orbit M11 buffer is too small: ") +
            name);
    }
}
} // namespace

bool GpuHydraulicErosionConfig::IsValid() const noexcept
{
    const auto unit =
        [](const f32 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0F &&
                value <= 1.0F;
        };

    const auto positive =
        [](const f32 value)
        {
            return
                std::isfinite(value) &&
                value > 0.0F;
        };

    const auto nonnegative =
        [](const f32 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0F;
        };

    return
        positive(timeStepSeconds) &&
        nonnegative(
            uniformRainfallMetersPerSecond) &&
        positive(
            gravityMetersPerSecondSquared) &&
        positive(
            pipeCrossSectionSquareMeters) &&
        nonnegative(
            sedimentCapacityCoefficient) &&
        nonnegative(
            maximumSedimentConcentrationKgPerCubicMeter) &&
        nonnegative(
            erosionRatePerSecond) &&
        nonnegative(
            depositionRatePerSecond) &&
        nonnegative(
            maximumErosionDepthPerStepMeters) &&
        nonnegative(
            maximumDepositionDepthPerStepMeters) &&
        unit(regolithMobility) &&
        unit(soilMobility) &&
        unit(sandMobility) &&
        unit(debrisMobility) &&
        positive(
            regolithDensityKgPerCubicMeter) &&
        positive(
            soilDensityKgPerCubicMeter) &&
        positive(
            sandDensityKgPerCubicMeter) &&
        positive(
            debrisDensityKgPerCubicMeter) &&
        nonnegative(
            infiltrationMetersPerSecond) &&
        positive(
            moistureCapacityDepthMeters) &&
        nonnegative(
            evaporationRatePerSecond);
}

GpuHydraulicErosionPage::GpuHydraulicErosionPage(
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
            "Orbit M11 GPU hydraulic solver requires geology capacity.");
    }

    initializePipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM11InitializeStateShader,
            3U,
            5U);

    rainFluxPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM11RainFluxShader,
            6U,
            5U,
            2U);

    waterVelocityPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM11WaterVelocityShader,
            3U,
            4U);

    erodeDepositPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM11ErodeDepositShader,
            17U,
            5U,
            3U);

    sedimentTransportPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM11SedimentTransportShader,
            3U,
            4U);

    infiltrationPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM11InfiltrationEvaporationShader,
            5U,
            2U,
            2U);

    snapshotPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM11SnapshotMaterialShader,
            1U,
            1U,
            3U);

    water_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kScalarBytesPerCell);

    waterScratch_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kScalarBytesPerCell);

    fluxA_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kFluxBytesPerCell);

    fluxB_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kFluxBytesPerCell);

    velocity_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kVelocityBytesPerCell);

    sedimentA_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kScalarBytesPerCell);

    sedimentB_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kScalarBytesPerCell);

    sedimentLocal_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kScalarBytesPerCell);

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

    zeroRainfall_ =
        device.CreateBuffer({
            .sizeBytes =
                maxCells *
                kScalarBytesPerCell,
            .usage =
                rhi::BufferUsage::Structured,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::ShaderResource
        });

    std::byte* rainfall =
        zeroRainfall_->Map();

    std::memset(
        rainfall,
        0,
        static_cast<std::size_t>(
            zeroRainfall_->SizeBytes()));

    zeroRainfall_->Unmap();

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
}

GpuHydraulicErosionPage::~GpuHydraulicErosionPage() = default;

void GpuHydraulicErosionPage::UploadGeologyTable(
    const terrain_geology::GeologicalMaterialGpuTable& table)
{
    if (table.materials.empty() ||
        table.materials.size() !=
            table.rockTypes.size() ||
        table.materials.size() >
            maxGeologyEntries_)
    {
        throw std::invalid_argument(
            "Orbit M11 geology table is empty, inconsistent, or exceeds "
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

void GpuHydraulicErosionPage::Reset(
    rhi::CommandList& commandList,
    const u32 resolution,
    const f32 initialWaterDepthMeters,
    const f32 initialSedimentKgPerSquareMeter)
{
    if (resolution == 0U ||
        resolution >
            maxResolution_ ||
        !std::isfinite(
            initialWaterDepthMeters) ||
        initialWaterDepthMeters <
            0.0F ||
        !std::isfinite(
            initialSedimentKgPerSquareMeter) ||
        initialSedimentKgPerSquareMeter <
            0.0F)
    {
        throw std::invalid_argument(
            "Orbit M11 reset parameters are invalid.");
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
        *water_);

    commandList.SetComputeBuffer(
        1U,
        *fluxA_);

    commandList.SetComputeBuffer(
        2U,
        *velocity_);

    commandList.SetComputeBuffer(
        3U,
        *sedimentA_);

    commandList.SetComputeBuffer(
        4U,
        *sedimentLocal_);

    std::array<u32, 3> constants{
        resolution,
        std::bit_cast<u32>(
            initialWaterDepthMeters),
        std::bit_cast<u32>(
            initialSedimentKgPerSquareMeter)
    };

    commandList.SetComputeConstants(
        constants);

    commandList.Dispatch(
        groupCount,
        groupCount,
        1U);

    for (rhi::Buffer* buffer :
         std::array<rhi::Buffer*, 5>{
             water_.get(),
             fluxA_.get(),
             velocity_.get(),
             sedimentA_.get(),
             sedimentLocal_.get()})
    {
        commandList.UavBarrier(
            *buffer);
    }

    activeResolution_ =
        resolution;

    initialized_ = true;
    fluxParity_ = false;
    sedimentParity_ = false;
    readbackRecorded_ = false;
}

void GpuHydraulicErosionPage::DispatchSteps(
    rhi::CommandList& commandList,
    const u32 resolution,
    const f32 spacingMeters,
    const u32 stepCount,
    const GpuHydraulicErosionConfig& config,
    GpuMaterialColumnResources& materialColumn,
    rhi::Buffer* rainfallRateMetersPerSecond)
{
    if (!config.IsValid() ||
        resolution == 0U ||
        resolution >
            maxResolution_ ||
        !std::isfinite(spacingMeters) ||
        spacingMeters <= 0.0F ||
        materialColumn.Resolution() !=
            resolution)
    {
        throw std::invalid_argument(
            "Orbit M11 GPU hydraulic dispatch is invalid.");
    }

    if (!geologyUploaded_)
    {
        throw std::logic_error(
            "Orbit M11 GPU hydraulic dispatch requires an uploaded M02 "
            "geology table.");
    }

    if (!initialized_ ||
        activeResolution_ != resolution)
    {
        Reset(
            commandList,
            resolution);
    }

    if (stepCount == 0U)
    {
        return;
    }

    rhi::Buffer& rainfall =
        rainfallRateMetersPerSecond != nullptr
            ? *rainfallRateMetersPerSecond
            : *zeroRainfall_;

    const u64 scalarBytes =
        static_cast<u64>(
            resolution) *
        resolution *
        kScalarBytesPerCell;

    RequireBufferSize(
        rainfall,
        scalarBytes,
        "rainfallRate");

    const u32 groupCount =
        (resolution +
         kThreadGroupSize -
         1U) /
        kThreadGroupSize;

    for (u32 step = 0U;
         step < stepCount;
         ++step)
    {
        rhi::Buffer* fluxIn =
            fluxParity_
                ? fluxB_.get()
                : fluxA_.get();

        rhi::Buffer* fluxOut =
            fluxParity_
                ? fluxA_.get()
                : fluxB_.get();

        rhi::Buffer* sedimentIn =
            sedimentParity_
                ? sedimentB_.get()
                : sedimentA_.get();

        rhi::Buffer* sedimentOut =
            sedimentParity_
                ? sedimentA_.get()
                : sedimentB_.get();

        // 1. Rain + virtual-pipe flux.
        commandList.SetComputePipeline(
            *rainFluxPipeline_);

        commandList.SetComputeBuffer(
            0U,
            *water_);

        commandList.SetComputeBuffer(
            1U,
            *fluxIn);

        commandList.SetComputeBuffer(
            2U,
            rainfall);

        commandList.SetComputeBuffer(
            3U,
            *waterScratch_);

        commandList.SetComputeBuffer(
            4U,
            *fluxOut);

        commandList.SetComputeStorageTexture(
            0U,
            materialColumn.
                BedrockHeight());

        commandList.SetComputeStorageTexture(
            1U,
            materialColumn.
                LooseMaterials());

        std::array<u32, 6>
            fluxConstants{};

        fluxConstants[0] =
            resolution;

        fluxConstants[1] =
            std::bit_cast<u32>(
                spacingMeters);

        fluxConstants[2] =
            std::bit_cast<u32>(
                config.
                    timeStepSeconds);

        fluxConstants[3] =
            std::bit_cast<u32>(
                config.
                    uniformRainfallMetersPerSecond);

        fluxConstants[4] =
            std::bit_cast<u32>(
                config.
                    gravityMetersPerSecondSquared);

        fluxConstants[5] =
            std::bit_cast<u32>(
                config.
                    pipeCrossSectionSquareMeters);

        commandList.SetComputeConstants(
            fluxConstants);

        commandList.Dispatch(
            groupCount,
            groupCount,
            1U);

        commandList.UavBarrier(
            *waterScratch_);

        commandList.UavBarrier(
            *fluxOut);

        // 2. Continuity + velocity.
        commandList.SetComputePipeline(
            *waterVelocityPipeline_);

        commandList.SetComputeBuffer(
            0U,
            *waterScratch_);

        commandList.SetComputeBuffer(
            1U,
            *fluxOut);

        commandList.SetComputeBuffer(
            2U,
            *water_);

        commandList.SetComputeBuffer(
            3U,
            *velocity_);

        const std::array<u32, 3>
            waterConstants{
                resolution,
                std::bit_cast<u32>(
                    spacingMeters),
                std::bit_cast<u32>(
                    config.
                        timeStepSeconds)
            };

        commandList.SetComputeConstants(
            waterConstants);

        commandList.Dispatch(
            groupCount,
            groupCount,
            1U);

        commandList.UavBarrier(
            *water_);

        commandList.UavBarrier(
            *velocity_);

        // 3. Capacity-controlled M08 material exchange.
        commandList.SetComputePipeline(
            *erodeDepositPipeline_);

        commandList.SetComputeBuffer(
            0U,
            *water_);

        commandList.SetComputeBuffer(
            1U,
            *velocity_);

        commandList.SetComputeBuffer(
            2U,
            *sedimentIn);

        commandList.SetComputeBuffer(
            3U,
            *geologyTable_);

        commandList.SetComputeBuffer(
            4U,
            *sedimentLocal_);

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
                GeologicalMaterial());

        std::array<u32, 17>
            erosionConstants{};

        erosionConstants[0] =
            resolution;

        const std::array<f32, 16>
            erosionFloats{
                spacingMeters,
                config.timeStepSeconds,
                config.sedimentCapacityCoefficient,
                config.maximumSedimentConcentrationKgPerCubicMeter,
                config.erosionRatePerSecond,
                config.depositionRatePerSecond,
                config.maximumErosionDepthPerStepMeters,
                config.maximumDepositionDepthPerStepMeters,
                config.regolithMobility,
                config.soilMobility,
                config.sandMobility,
                config.debrisMobility,
                config.regolithDensityKgPerCubicMeter,
                config.soilDensityKgPerCubicMeter,
                config.sandDensityKgPerCubicMeter,
                config.debrisDensityKgPerCubicMeter
            };

        for (u32 i = 0U;
             i <
                 erosionFloats.size();
             ++i)
        {
            erosionConstants[i + 1U] =
                std::bit_cast<u32>(
                    erosionFloats[i]);
        }

        commandList.SetComputeConstants(
            erosionConstants);

        commandList.Dispatch(
            groupCount,
            groupCount,
            1U);

        commandList.UavBarrier(
            *sedimentLocal_);

        commandList.UavBarrier(
            materialColumn.
                BedrockHeight());

        commandList.UavBarrier(
            materialColumn.
                LooseMaterials());

        // 4. Conservative sediment transport.
        commandList.SetComputePipeline(
            *sedimentTransportPipeline_);

        commandList.SetComputeBuffer(
            0U,
            *sedimentLocal_);

        commandList.SetComputeBuffer(
            1U,
            *waterScratch_);

        commandList.SetComputeBuffer(
            2U,
            *fluxOut);

        commandList.SetComputeBuffer(
            3U,
            *sedimentOut);

        const std::array<u32, 3>
            transportConstants{
                resolution,
                std::bit_cast<u32>(
                    spacingMeters),
                std::bit_cast<u32>(
                    config.
                        timeStepSeconds)
            };

        commandList.SetComputeConstants(
            transportConstants);

        commandList.Dispatch(
            groupCount,
            groupCount,
            1U);

        commandList.UavBarrier(
            *sedimentOut);

        // 5. Infiltration/M08 moisture + evaporation.
        commandList.SetComputePipeline(
            *infiltrationPipeline_);

        commandList.SetComputeBuffer(
            0U,
            *water_);

        commandList.SetComputeBuffer(
            1U,
            *geologyTable_);

        commandList.SetComputeStorageTexture(
            0U,
            materialColumn.
                MoistureProcess());

        commandList.SetComputeStorageTexture(
            1U,
            materialColumn.
                GeologicalMaterial());

        const std::array<u32, 5>
            infiltrationConstants{
                resolution,
                std::bit_cast<u32>(
                    config.
                        timeStepSeconds),
                std::bit_cast<u32>(
                    config.
                        infiltrationMetersPerSecond),
                std::bit_cast<u32>(
                    config.
                        moistureCapacityDepthMeters),
                std::bit_cast<u32>(
                    config.
                        evaporationRatePerSecond)
            };

        commandList.SetComputeConstants(
            infiltrationConstants);

        commandList.Dispatch(
            groupCount,
            groupCount,
            1U);

        commandList.UavBarrier(
            *water_);

        commandList.UavBarrier(
            materialColumn.
                MoistureProcess());

        fluxParity_ =
            !fluxParity_;

        sedimentParity_ =
            !sedimentParity_;
    }

    readbackRecorded_ = false;
}

void GpuHydraulicErosionPage::RecordMaterialReadback(
    rhi::CommandList& commandList,
    const u32 resolution,
    GpuMaterialColumnResources& materialColumn)
{
    if (!initialized_ ||
        resolution !=
            activeResolution_ ||
        resolution >
            maxResolution_ ||
        materialColumn.Resolution() !=
            resolution)
    {
        throw std::invalid_argument(
            "Orbit M11 material readback does not match the active GPU page.");
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

void GpuHydraulicErosionPage::ApplyMaterialReadbackToCpu(
    terrain_material_column::MaterialColumnPage& page)
{
    if (!readbackRecorded_ ||
        page.Resolution() !=
            activeResolution_)
    {
        throw std::logic_error(
            "Orbit M11 CPU material sync requires a completed matching "
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
GpuHydraulicErosionPage::WaterDepth() noexcept
{
    return *water_;
}

rhi::Buffer&
GpuHydraulicErosionPage::Velocity() noexcept
{
    return *velocity_;
}

rhi::Buffer&
GpuHydraulicErosionPage::SuspendedSediment() noexcept
{
    return sedimentParity_
        ? *sedimentB_
        : *sedimentA_;
}

rhi::Buffer&
GpuHydraulicErosionPage::CurrentFlux() noexcept
{
    return fluxParity_
        ? *fluxB_
        : *fluxA_;
}
} // namespace orbit::terrain_gpu
