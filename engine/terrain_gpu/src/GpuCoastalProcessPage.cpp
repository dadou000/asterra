#include <orbit/terrain_gpu/GpuCoastalProcessPage.hpp>

#include "M17CoastalCompute.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace orbit::terrain_gpu
{
namespace
{
constexpr u32 kThreadGroupSize = 8U;

constexpr u64 kScalarBytesPerCell = 4U;
constexpr u64 kMomentumBytesPerCell = 8U;
constexpr u64 kMaskBytesPerCell = 4U;
constexpr u64 kSedimentBytesPerCell = 16U;

constexpr u64 kBoundaryRecordBytes = 20U;
constexpr u64 kMaterialSnapshotBytesPerCell = 24U;
constexpr u64 kWaterReadbackBytesPerCell = 20U;

[[nodiscard]] u64 CheckedCellCount(
    const u32 resolution)
{
    if (resolution == 0U)
    {
        throw std::invalid_argument(
            "Orbit M17 GPU coastal solver requires non-zero resolution.");
    }

    const u64 count =
        static_cast<u64>(resolution) *
        static_cast<u64>(resolution);

    if (count >
        std::numeric_limits<u32>::max())
    {
        throw std::invalid_argument(
            "Orbit M17 GPU coastal page exceeds 32-bit cell addressing.");
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
            "Orbit failed to compile an M17 coastal compute shader.");
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

[[nodiscard]] u32 FloatBits(
    const f32 value) noexcept
{
    return std::bit_cast<u32>(
        value);
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
                "Orbit M17 buffer is too small: ") +
            name);
    }
}

[[nodiscard]] f64 SedimentTotal(
    const std::array<f32, 3>& mass) noexcept
{
    return
        static_cast<f64>(mass[0]) +
        static_cast<f64>(mass[1]) +
        static_cast<f64>(mass[2]);
}
} // namespace

bool GpuCoastalProcessConfig::IsValid() const noexcept
{
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

    const auto unit =
        [](const f32 value)
        {
            return
                std::isfinite(value) &&
                value >= 0.0F &&
                value <= 1.0F;
        };

    const f32 waveDirectionLength =
        std::hypot(
            waveDirectionX,
            waveDirectionY);

    return
        positive(timeStepSeconds) &&
        positive(maximumExpectedWaterDepthMeters) &&
        positive(gravityMetersPerSecondSquared) &&
        positive(cflNumber) &&
        cflNumber <= 0.5F &&
        std::isfinite(seaLevelMeters) &&
        positive(wetThresholdMeters) &&
        nonnegative(dryThresholdMeters) &&
        dryThresholdMeters <
            wetThresholdMeters &&
        nonnegative(manningRoughness) &&
        positive(maximumVelocityMetersPerSecond) &&
        nonnegative(waveAmplitudeMeters) &&
        positive(wavePeriodSeconds) &&
        std::isfinite(wavePhaseRadians) &&
        std::isfinite(waveDirectionX) &&
        std::isfinite(waveDirectionY) &&
        (waveAmplitudeMeters == 0.0F ||
         waveDirectionLength > 1.0e-6F) &&
        positive(activeDepthMeters) &&
        positive(referenceEnergySquareMetersPerSecondSquared) &&
        nonnegative(erosionMetersPerSecondAtReference) &&
        nonnegative(maximumErosionDepthPerStepMeters) &&
        unit(sandBedloadFraction) &&
        unit(finesBedloadFraction) &&
        unit(coarseDebrisBedloadFraction) &&
        nonnegative(transportRate) &&
        unit(maximumTransportFractionPerStep) &&
        nonnegative(depositionVelocityThresholdMetersPerSecond) &&
        nonnegative(depositionRatePerSecond) &&
        nonnegative(shorelineDepositionMultiplier) &&
        positive(regolithDensityKgPerCubicMeter) &&
        positive(soilDensityKgPerCubicMeter) &&
        positive(sandDensityKgPerCubicMeter) &&
        positive(debrisDensityKgPerCubicMeter) &&
        unit(coastalBedrockSandFraction);
}

GpuCoastalProcessPage::GpuCoastalProcessPage(
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
            "Orbit M17 GPU coastal solver requires geology capacity.");
    }

    initializePipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM17InitializeShader,
            2U,
            2U,
            2U);

    advancePipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM17AdvanceShader,
            14U,
            5U,
            2U);

    shorelinePipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM17ShorelineShader,
            2U,
            3U);

    sedimentExchangePipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM17SedimentExchangeShader,
            17U,
            7U,
            3U);

    sedimentTransportPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM17SedimentTransportShader,
            6U,
            8U);

    sedimentDepositPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM17SedimentDepositShader,
            9U,
            7U,
            1U);

    snapshotPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM17SnapshotMaterialShader,
            1U,
            1U,
            3U);

    waterA_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kScalarBytesPerCell);

    waterB_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kScalarBytesPerCell);

    momentumA_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kMomentumBytesPerCell);

    momentumB_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kMomentumBytesPerCell);

    wet_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kMaskBytesPerCell);

    shoreline_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kMaskBytesPerCell);

    waterborneScratch_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kSedimentBytesPerCell);

    surfaceScratch_ =
        CreateGpuBuffer(
            device,
            maxCells *
                kSedimentBytesPerCell);

    boundaryWaterborneExport_ =
        CreateGpuBuffer(
            device,
            static_cast<u64>(
                maxResolution_) *
                4U *
                kSedimentBytesPerCell);

    boundarySurfaceExport_ =
        CreateGpuBuffer(
            device,
            static_cast<u64>(
                maxResolution_) *
                4U *
                kSedimentBytesPerCell);

    boundary_ =
        device.CreateBuffer({
            .sizeBytes =
                static_cast<u64>(
                    maxResolution_) *
                4U *
                kBoundaryRecordBytes,
            .usage =
                rhi::BufferUsage::Structured,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::ShaderResource
        });

    zeroProtection_ =
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

    std::byte* protection =
        zeroProtection_->Map();

    std::memset(
        protection,
        0,
        static_cast<std::size_t>(
            zeroProtection_->SizeBytes()));

    zeroProtection_->Unmap();

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

    waterReadback_ =
        device.CreateBuffer({
            .sizeBytes =
                maxCells *
                kWaterReadbackBytesPerCell,
            .usage =
                rhi::BufferUsage::Generic,
            .memory =
                rhi::MemoryUsage::HostReadback,
            .initialState =
                rhi::ResourceState::CopyDestination
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

GpuCoastalProcessPage::~GpuCoastalProcessPage() = default;

void GpuCoastalProcessPage::UploadGeologyTable(
    const terrain_geology::GeologicalMaterialGpuTable& table)
{
    if (table.materials.empty() ||
        table.materials.size() !=
            table.rockTypes.size() ||
        table.materials.size() >
            maxGeologyEntries_)
    {
        throw std::invalid_argument(
            "Orbit M17 geology table is empty, inconsistent, or exceeds "
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

void GpuCoastalProcessPage::UploadBoundary(
    const u32 resolution,
    const terrain_water::CoastalBoundaryState& boundary)
{
    if (resolution == 0U ||
        resolution >
            maxResolution_ ||
        !boundary.IsComplete(
            resolution))
    {
        throw std::invalid_argument(
            "Orbit M17 GPU coastal boundary is invalid.");
    }

    std::byte* mapped =
        boundary_->Map();

    std::memset(
        mapped,
        0,
        static_cast<std::size_t>(
            boundary_->SizeBytes()));

    const bool allClosed =
        boundary.north.empty() &&
        boundary.east.empty() &&
        boundary.south.empty() &&
        boundary.west.empty();

    const auto side =
        [&boundary](
            const u32 sideIndex)
            -> const std::vector<
                terrain_water::
                    CoastalBoundaryCell>&
        {
            switch (sideIndex)
            {
            case 0U:
                return boundary.north;
            case 1U:
                return boundary.east;
            case 2U:
                return boundary.south;
            default:
                return boundary.west;
            }
        };

    if (!allClosed)
    {
        for (u32 sideIndex = 0U;
             sideIndex < 4U;
             ++sideIndex)
        {
            const auto& cells =
                side(
                    sideIndex);

            for (u32 i = 0U;
                 i < resolution;
                 ++i)
            {
                const auto& cell =
                    cells[i];

                const std::array<u32, 5>
                    record{
                        FloatBits(
                            cell.
                                bedElevationMeters),
                        FloatBits(
                            cell.
                                waterSurfaceElevationMeters),
                        FloatBits(
                            cell.
                                velocityEastMetersPerSecond),
                        FloatBits(
                            cell.
                                velocitySouthMetersPerSecond),
                        static_cast<u32>(
                            cell.mode)
                    };

                const u64 recordIndex =
                    static_cast<u64>(
                        sideIndex) *
                        resolution +
                    i;

                std::memcpy(
                    mapped +
                        recordIndex *
                            kBoundaryRecordBytes,
                    record.data(),
                    kBoundaryRecordBytes);
            }
        }
    }

    boundary_->Unmap();

    boundaryResolution_ =
        resolution;

    boundaryUploaded_ = true;
}

void GpuCoastalProcessPage::Reset(
    rhi::CommandList& commandList,
    const u32 resolution,
    const GpuCoastalProcessConfig& config,
    GpuMaterialColumnResources& materialColumn)
{
    if (!config.IsValid() ||
        resolution == 0U ||
        resolution >
            maxResolution_ ||
        materialColumn.Resolution() !=
            resolution)
    {
        throw std::invalid_argument(
            "Orbit M17 GPU coastal reset parameters are invalid.");
    }

    const u32 groups =
        (resolution +
         kThreadGroupSize -
         1U) /
        kThreadGroupSize;

    commandList.SetComputePipeline(
        *initializePipeline_);

    commandList.SetComputeBuffer(
        0U,
        *waterA_);

    commandList.SetComputeBuffer(
        1U,
        *momentumA_);

    commandList.SetComputeStorageTexture(
        0U,
        materialColumn.
            BedrockHeight());

    commandList.SetComputeStorageTexture(
        1U,
        materialColumn.
            LooseMaterials());

    const std::array<u32, 2>
        constants{
            resolution,
            FloatBits(
                config.
                    seaLevelMeters)
        };

    commandList.SetComputeConstants(
        constants);

    commandList.Dispatch(
        groups,
        groups,
        1U);

    commandList.UavBarrier(
        *waterA_);

    commandList.UavBarrier(
        *momentumA_);

    commandList.SetComputePipeline(
        *shorelinePipeline_);

    commandList.SetComputeBuffer(
        0U,
        *waterA_);

    commandList.SetComputeBuffer(
        1U,
        *wet_);

    commandList.SetComputeBuffer(
        2U,
        *shoreline_);

    const std::array<u32, 2>
        shorelineConstants{
            resolution,
            FloatBits(
                config.
                    wetThresholdMeters)
        };

    commandList.SetComputeConstants(
        shorelineConstants);

    commandList.Dispatch(
        groups,
        groups,
        1U);

    commandList.UavBarrier(
        *wet_);

    commandList.UavBarrier(
        *shoreline_);

    activeResolution_ =
        resolution;

    activeConfig_ =
        config;

    stateParity_ = false;
    initialized_ = true;
    elapsedSeconds_ = 0.0F;
    readbackRecorded_ = false;
}

void GpuCoastalProcessPage::DispatchSteps(
    rhi::CommandList& commandList,
    const u32 resolution,
    const f32 spacingMeters,
    const u32 stepCount,
    const GpuCoastalProcessConfig& config,
    GpuMaterialColumnResources& materialColumn,
    GpuSedimentExchangeResources& sediment,
    rhi::Buffer* protection)
{
    if (!config.IsValid() ||
        resolution == 0U ||
        resolution >
            maxResolution_ ||
        !std::isfinite(
            spacingMeters) ||
        spacingMeters <= 0.0F ||
        materialColumn.Resolution() !=
            resolution ||
        sediment.Resolution() !=
            resolution)
    {
        throw std::invalid_argument(
            "Orbit M17 GPU coastal dispatch is invalid.");
    }

    if (!geologyUploaded_)
    {
        throw std::logic_error(
            "Orbit M17 GPU coastal dispatch requires an uploaded M02 "
            "geology table.");
    }

    const f32 maximumSignal =
        config.
            maximumVelocityMetersPerSecond +
        std::sqrt(
            config.
                gravityMetersPerSecondSquared *
            config.
                maximumExpectedWaterDepthMeters);

    const f32 maximumStableTimeStep =
        config.
            cflNumber *
        spacingMeters /
        std::max(
            maximumSignal,
            1.0e-6F);

    if (config.timeStepSeconds >
        maximumStableTimeStep *
            1.0001F)
    {
        throw std::invalid_argument(
            "Orbit M17 GPU coastal timestep violates its declared CFL/depth "
            "stability envelope.");
    }

    if (!boundaryUploaded_ ||
        boundaryResolution_ !=
            resolution)
    {
        UploadBoundary(
            resolution,
            {});
    }

    if (!initialized_ ||
        activeResolution_ !=
            resolution)
    {
        Reset(
            commandList,
            resolution,
            config,
            materialColumn);
    }

    if (stepCount == 0U)
    {
        return;
    }

    rhi::Buffer& protectionBuffer =
        protection != nullptr
            ? *protection
            : *zeroProtection_;

    RequireBufferSize(
        protectionBuffer,
        static_cast<u64>(
            resolution) *
            resolution *
            kScalarBytesPerCell,
        "protection");

    const u32 groups =
        (resolution +
         kThreadGroupSize -
         1U) /
        kThreadGroupSize;

    const math::Double2 waveDirection{
        static_cast<f64>(
            config.waveDirectionX),
        static_cast<f64>(
            config.waveDirectionY)
    };

    const f64 waveLength =
        std::hypot(
            waveDirection.x,
            waveDirection.y);

    const f32 normalizedWaveX =
        waveLength >
                1.0e-12
            ? static_cast<f32>(
                waveDirection.x /
                waveLength)
            : 1.0F;

    const f32 normalizedWaveY =
        waveLength >
                1.0e-12
            ? static_cast<f32>(
                waveDirection.y /
                waveLength)
            : 0.0F;

    for (u32 step = 0U;
         step < stepCount;
         ++step)
    {
        rhi::Buffer* waterIn =
            stateParity_
                ? waterB_.get()
                : waterA_.get();

        rhi::Buffer* momentumIn =
            stateParity_
                ? momentumB_.get()
                : momentumA_.get();

        rhi::Buffer* waterOut =
            stateParity_
                ? waterA_.get()
                : waterB_.get();

        rhi::Buffer* momentumOut =
            stateParity_
                ? momentumA_.get()
                : momentumB_.get();

        // 1. Positivity-clamped finite-volume shallow-water step.
        commandList.SetComputePipeline(
            *advancePipeline_);

        commandList.SetComputeBuffer(
            0U,
            *waterIn);

        commandList.SetComputeBuffer(
            1U,
            *momentumIn);

        commandList.SetComputeBuffer(
            2U,
            *boundary_);

        commandList.SetComputeBuffer(
            3U,
            *waterOut);

        commandList.SetComputeBuffer(
            4U,
            *momentumOut);

        commandList.SetComputeStorageTexture(
            0U,
            materialColumn.
                BedrockHeight());

        commandList.SetComputeStorageTexture(
            1U,
            materialColumn.
                LooseMaterials());

        const std::array<f32, 13>
            advanceFloats{
                spacingMeters,
                config.
                    timeStepSeconds,
                config.
                    gravityMetersPerSecondSquared,
                config.
                    dryThresholdMeters,
                config.
                    wetThresholdMeters,
                config.
                    manningRoughness,
                config.
                    maximumVelocityMetersPerSecond,
                elapsedSeconds_,
                config.
                    waveAmplitudeMeters,
                config.
                    wavePeriodSeconds,
                config.
                    wavePhaseRadians,
                normalizedWaveX,
                normalizedWaveY
            };

        std::array<u32, 14>
            advanceConstants{};

        advanceConstants[0] =
            resolution;

        for (u32 i = 0U;
             i <
                 advanceFloats.size();
             ++i)
        {
            advanceConstants[
                i + 1U] =
                    FloatBits(
                        advanceFloats[i]);
        }

        commandList.SetComputeConstants(
            advanceConstants);

        commandList.Dispatch(
            groups,
            groups,
            1U);

        commandList.UavBarrier(
            *waterOut);

        commandList.UavBarrier(
            *momentumOut);

        stateParity_ =
            !stateParity_;

        rhi::Buffer& currentWater =
            stateParity_
                ? *waterB_
                : *waterA_;

        rhi::Buffer& currentMomentum =
            stateParity_
                ? *momentumB_
                : *momentumA_;

        // 2. Terrain-responsive wet/dry and shoreline classification.
        commandList.SetComputePipeline(
            *shorelinePipeline_);

        commandList.SetComputeBuffer(
            0U,
            currentWater);

        commandList.SetComputeBuffer(
            1U,
            *wet_);

        commandList.SetComputeBuffer(
            2U,
            *shoreline_);

        const std::array<u32, 2>
            shorelineConstants{
                resolution,
                FloatBits(
                    config.
                        wetThresholdMeters)
            };

        commandList.SetComputeConstants(
            shorelineConstants);

        commandList.Dispatch(
            groups,
            groups,
            1U);

        commandList.UavBarrier(
            *wet_);

        commandList.UavBarrier(
            *shoreline_);

        // 3. Surf-zone M08 pickup -> typed M14 waterborne/bedload lanes.
        commandList.SetComputePipeline(
            *sedimentExchangePipeline_);

        commandList.SetComputeBuffer(
            0U,
            currentWater);

        commandList.SetComputeBuffer(
            1U,
            currentMomentum);

        commandList.SetComputeBuffer(
            2U,
            *shoreline_);

        commandList.SetComputeBuffer(
            3U,
            *geologyTable_);

        commandList.SetComputeBuffer(
            4U,
            protectionBuffer);

        commandList.SetComputeBuffer(
            5U,
            sediment.
                Waterborne());

        commandList.SetComputeBuffer(
            6U,
            sediment.
                SurfaceMobile());

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

        const std::array<f32, 16>
            exchangeFloats{
                config.
                    timeStepSeconds,
                config.
                    seaLevelMeters,
                config.
                    gravityMetersPerSecondSquared,
                config.
                    wetThresholdMeters,
                config.
                    activeDepthMeters,
                config.
                    referenceEnergySquareMetersPerSecondSquared,
                config.
                    erosionMetersPerSecondAtReference,
                config.
                    maximumErosionDepthPerStepMeters,
                config.
                    sandBedloadFraction,
                config.
                    finesBedloadFraction,
                config.
                    coarseDebrisBedloadFraction,
                config.
                    regolithDensityKgPerCubicMeter,
                config.
                    soilDensityKgPerCubicMeter,
                config.
                    sandDensityKgPerCubicMeter,
                config.
                    debrisDensityKgPerCubicMeter,
                config.
                    coastalBedrockSandFraction
            };

        std::array<u32, 17>
            exchangeConstants{};

        exchangeConstants[0] =
            resolution;

        for (u32 i = 0U;
             i <
                 exchangeFloats.size();
             ++i)
        {
            exchangeConstants[
                i + 1U] =
                    FloatBits(
                        exchangeFloats[i]);
        }

        commandList.SetComputeConstants(
            exchangeConstants);

        commandList.Dispatch(
            groups,
            groups,
            1U);

        commandList.UavBarrier(
            currentWater);

        commandList.UavBarrier(
            sediment.
                Waterborne());

        commandList.UavBarrier(
            sediment.
                SurfaceMobile());

        commandList.UavBarrier(
            materialColumn.
                BedrockHeight());

        commandList.UavBarrier(
            materialColumn.
                LooseMaterials());

        // 4. Conservative gather transport + explicit typed edge exports.
        commandList.SetComputePipeline(
            *sedimentTransportPipeline_);

        commandList.SetComputeBuffer(
            0U,
            currentWater);

        commandList.SetComputeBuffer(
            1U,
            currentMomentum);

        commandList.SetComputeBuffer(
            2U,
            sediment.
                Waterborne());

        commandList.SetComputeBuffer(
            3U,
            sediment.
                SurfaceMobile());

        commandList.SetComputeBuffer(
            4U,
            *waterborneScratch_);

        commandList.SetComputeBuffer(
            5U,
            *surfaceScratch_);

        commandList.SetComputeBuffer(
            6U,
            *boundaryWaterborneExport_);

        commandList.SetComputeBuffer(
            7U,
            *boundarySurfaceExport_);

        const std::array<u32, 6>
            transportConstants{
                resolution,
                FloatBits(
                    spacingMeters),
                FloatBits(
                    config.
                        timeStepSeconds),
                FloatBits(
                    config.
                        transportRate),
                FloatBits(
                    config.
                        maximumTransportFractionPerStep),
                FloatBits(
                    config.
                        wetThresholdMeters)
            };

        commandList.SetComputeConstants(
            transportConstants);

        commandList.Dispatch(
            groups,
            groups,
            1U);

        commandList.UavBarrier(
            *waterborneScratch_);

        commandList.UavBarrier(
            *surfaceScratch_);

        commandList.UavBarrier(
            *boundaryWaterborneExport_);

        commandList.UavBarrier(
            *boundarySurfaceExport_);

        // 5. Low-energy shoreline deposition back to M08 and commit M14.
        commandList.SetComputePipeline(
            *sedimentDepositPipeline_);

        commandList.SetComputeBuffer(
            0U,
            currentWater);

        commandList.SetComputeBuffer(
            1U,
            currentMomentum);

        commandList.SetComputeBuffer(
            2U,
            *shoreline_);

        commandList.SetComputeBuffer(
            3U,
            *waterborneScratch_);

        commandList.SetComputeBuffer(
            4U,
            *surfaceScratch_);

        commandList.SetComputeBuffer(
            5U,
            sediment.
                Waterborne());

        commandList.SetComputeBuffer(
            6U,
            sediment.
                SurfaceMobile());

        commandList.SetComputeStorageTexture(
            0U,
            materialColumn.
                LooseMaterials());

        const std::array<u32, 9>
            depositConstants{
                resolution,
                FloatBits(
                    config.
                        timeStepSeconds),
                FloatBits(
                    config.
                        wetThresholdMeters),
                FloatBits(
                    config.
                        depositionVelocityThresholdMetersPerSecond),
                FloatBits(
                    config.
                        depositionRatePerSecond),
                FloatBits(
                    config.
                        shorelineDepositionMultiplier),
                FloatBits(
                    config.
                        soilDensityKgPerCubicMeter),
                FloatBits(
                    config.
                        sandDensityKgPerCubicMeter),
                FloatBits(
                    config.
                        debrisDensityKgPerCubicMeter)
            };

        commandList.SetComputeConstants(
            depositConstants);

        commandList.Dispatch(
            groups,
            groups,
            1U);

        commandList.UavBarrier(
            currentWater);

        commandList.UavBarrier(
            sediment.
                Waterborne());

        commandList.UavBarrier(
            sediment.
                SurfaceMobile());

        commandList.UavBarrier(
            materialColumn.
                LooseMaterials());

        elapsedSeconds_ +=
            config.
                timeStepSeconds;
    }

    activeConfig_ =
        config;

    readbackRecorded_ = false;
}

void GpuCoastalProcessPage::RecordReadback(
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
            "Orbit M17 coastal readback does not match the active GPU page.");
    }

    rhi::Buffer& currentWater =
        stateParity_
            ? *waterB_
            : *waterA_;

    rhi::Buffer& currentMomentum =
        stateParity_
            ? *momentumB_
            : *momentumA_;

    const u64 cells =
        static_cast<u64>(
            resolution) *
        resolution;

    const u64 waterBytes =
        cells *
        kScalarBytesPerCell;

    const u64 momentumBytes =
        cells *
        kMomentumBytesPerCell;

    const u64 maskBytes =
        cells *
        kMaskBytesPerCell;

    const std::array<
        std::pair<rhi::Buffer*, u64>,
        4>
        sources{{
            {&currentWater, waterBytes},
            {&currentMomentum, momentumBytes},
            {wet_.get(), maskBytes},
            {shoreline_.get(), maskBytes}
        }};

    u64 destinationOffset = 0U;

    for (const auto& [source, bytes] :
         sources)
    {
        commandList.Transition(
            *source,
            rhi::ResourceState::
                UnorderedAccess,
            rhi::ResourceState::
                CopySource);

        commandList.CopyBuffer(
            *source,
            0U,
            *waterReadback_,
            destinationOffset,
            bytes);

        commandList.Transition(
            *source,
            rhi::ResourceState::
                CopySource,
            rhi::ResourceState::
                UnorderedAccess);

        destinationOffset +=
            bytes;
    }

    const u32 groups =
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
        snapshotConstants{
            resolution
        };

    commandList.SetComputeConstants(
        snapshotConstants);

    commandList.Dispatch(
        groups,
        groups,
        1U);

    commandList.UavBarrier(
        *materialSnapshot_);

    const u64 materialBytes =
        cells *
        kMaterialSnapshotBytesPerCell;

    commandList.Transition(
        *materialSnapshot_,
        rhi::ResourceState::
            UnorderedAccess,
        rhi::ResourceState::
            CopySource);

    commandList.CopyBuffer(
        *materialSnapshot_,
        0U,
        *materialReadback_,
        0U,
        materialBytes);

    commandList.Transition(
        *materialSnapshot_,
        rhi::ResourceState::
            CopySource,
        rhi::ResourceState::
            UnorderedAccess);

    readbackRecorded_ = true;
}

void GpuCoastalProcessPage::ApplyWaterReadbackToCpu(
    terrain_water::CoastalWaterPage& page,
    const f64 spacingMeters)
{
    if (!readbackRecorded_ ||
        page.resolution !=
            activeResolution_ ||
        page.cells.size() !=
            static_cast<std::size_t>(
                activeResolution_) *
                activeResolution_ ||
        !std::isfinite(
            spacingMeters) ||
        spacingMeters <= 0.0)
    {
        throw std::logic_error(
            "Orbit M17 CPU water sync requires a completed matching "
            "RecordReadback submission and initialized CPU page.");
    }

    const u64 cells =
        static_cast<u64>(
            activeResolution_) *
        activeResolution_;

    const u64 waterBytes =
        cells *
        kScalarBytesPerCell;

    const u64 momentumBytes =
        cells *
        kMomentumBytesPerCell;

    const u64 maskBytes =
        cells *
        kMaskBytesPerCell;

    const u64 momentumOffset =
        waterBytes;

    const u64 wetOffset =
        momentumOffset +
        momentumBytes;

    const u64 shorelineOffset =
        wetOffset +
        maskBytes;

    const std::byte* mapped =
        waterReadback_->Map();

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

            f32 depth = 0.0F;

            std::array<f32, 2>
                momentum{};

            u32 wet = 0U;
            u32 shoreline = 0U;

            std::memcpy(
                &depth,
                mapped +
                    index *
                        kScalarBytesPerCell,
                sizeof(depth));

            std::memcpy(
                momentum.data(),
                mapped +
                    momentumOffset +
                    index *
                        kMomentumBytesPerCell,
                kMomentumBytesPerCell);

            std::memcpy(
                &wet,
                mapped +
                    wetOffset +
                    index *
                        kMaskBytesPerCell,
                sizeof(wet));

            std::memcpy(
                &shoreline,
                mapped +
                    shorelineOffset +
                    index *
                        kMaskBytesPerCell,
                sizeof(shoreline));

            auto& cell =
                page.At(
                    x,
                    y);

            cell.waterDepthMeters =
                std::max(
                    static_cast<f64>(
                        depth),
                    0.0);

            cell.
                momentumEastSquareMetersPerSecond =
                    static_cast<f64>(
                        momentum[0]);

            cell.
                momentumSouthSquareMetersPerSecond =
                    static_cast<f64>(
                        momentum[1]);

            cell.wet =
                wet != 0U;

            cell.shoreline =
                shoreline != 0U;

            if (cell.waterDepthMeters >
                static_cast<f64>(
                    activeConfig_.
                        dryThresholdMeters))
            {
                cell.
                    velocityMetersPerSecond = {
                        cell.
                            momentumEastSquareMetersPerSecond /
                            cell.
                                waterDepthMeters,
                        cell.
                            momentumSouthSquareMetersPerSecond /
                            cell.
                                waterDepthMeters
                    };
            }
            else
            {
                cell.
                    velocityMetersPerSecond = {};
            }

            cell.
                waterSurfaceElevationMeters =
                    cell.
                        bedElevationMeters +
                    cell.
                        waterDepthMeters;

            const f64 speed =
                std::hypot(
                    cell.
                        velocityMetersPerSecond.x,
                    cell.
                        velocityMetersPerSecond.y);

            cell.
                specificWaveCurrentEnergySquareMetersPerSecondSquared =
                    0.5 *
                        speed *
                        speed +
                    static_cast<f64>(
                        activeConfig_.
                            gravityMetersPerSecondSquared) *
                        std::abs(
                            cell.
                                waterSurfaceElevationMeters -
                            static_cast<f64>(
                                activeConfig_.
                                    seaLevelMeters));
        }
    }

    waterReadback_->Unmap();

    page.spacingMeters =
        spacingMeters;

    page.elapsedSeconds =
        elapsedSeconds_;

    page.lastTimeStepSeconds =
        activeConfig_.
            timeStepSeconds;
}

void GpuCoastalProcessPage::ApplyMaterialReadbackToCpu(
    terrain_material_column::MaterialColumnPage& page)
{
    if (!readbackRecorded_ ||
        page.Resolution() !=
            activeResolution_)
    {
        throw std::logic_error(
            "Orbit M17 CPU material sync requires a completed matching "
            "RecordReadback submission.");
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
GpuCoastalProcessPage::WaterDepth() noexcept
{
    return
        stateParity_
            ? *waterB_
            : *waterA_;
}

rhi::Buffer&
GpuCoastalProcessPage::Momentum() noexcept
{
    return
        stateParity_
            ? *momentumB_
            : *momentumA_;
}

rhi::Buffer&
GpuCoastalProcessPage::WetMask() noexcept
{
    return *wet_;
}

rhi::Buffer&
GpuCoastalProcessPage::ShorelineMask() noexcept
{
    return *shoreline_;
}

rhi::Buffer&
GpuCoastalProcessPage::BoundaryWaterborneExport() noexcept
{
    return
        *boundaryWaterborneExport_;
}

rhi::Buffer&
GpuCoastalProcessPage::BoundarySurfaceExport() noexcept
{
    return
        *boundarySurfaceExport_;
}
} // namespace orbit::terrain_gpu
