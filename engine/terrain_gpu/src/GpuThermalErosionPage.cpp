#include <orbit/terrain_gpu/GpuThermalErosionPage.hpp>

#include "M12ThermalCompute.hpp"

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
constexpr u64 kTransferBytesPerCell = 16U;
constexpr u64 kProtectionBytesPerCell = 4U;
constexpr u64 kMaterialSnapshotBytesPerCell = 24U;

[[nodiscard]] u64 CheckedCellCount(
    const u32 resolution)
{
    if (resolution == 0U)
    {
        throw std::invalid_argument(
            "Orbit M12 GPU thermal solver requires non-zero resolution.");
    }

    const u64 count =
        static_cast<u64>(resolution) *
        static_cast<u64>(resolution);

    if (count >
        std::numeric_limits<u32>::max())
    {
        throw std::invalid_argument(
            "Orbit M12 GPU thermal page exceeds 32-bit cell addressing.");
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
    const u32 storageTextures)
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
            "Orbit failed to compile an M12 thermal compute shader.");
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
    if (buffer.SizeBytes() <
        required)
    {
        throw std::invalid_argument(
            std::string(
                "Orbit M12 buffer is too small: ") +
            name);
    }
}
} // namespace

bool GpuThermalErosionConfig::IsValid() const noexcept
{
    const auto angle =
        [](const f32 value)
        {
            return
                std::isfinite(value) &&
                value > 0.0F &&
                value < 90.0F;
        };

    const auto positive =
        [](const f32 value)
        {
            return
                std::isfinite(value) &&
                value > 0.0F;
        };

    return
        angle(sandReposeDegrees) &&
        angle(debrisReposeDegrees) &&
        angle(regolithReposeDegrees) &&
        angle(soilReposeDegrees) &&
        angle(minimumBedrockFailureDegrees) &&
        std::isfinite(
            bedrockFailureAngleRangeDegrees) &&
        bedrockFailureAngleRangeDegrees >= 0.0F &&
        minimumBedrockFailureDegrees +
                bedrockFailureAngleRangeDegrees <
            90.0F &&
        std::isfinite(
            bedrockFractureRate) &&
        bedrockFractureRate >= 0.0F &&
        bedrockFractureRate <= 1.0F &&
        std::isfinite(relaxation) &&
        relaxation > 0.0F &&
        relaxation <= 1.0F &&
        std::isfinite(
            maximumTransferDepthPerIterationMeters) &&
        maximumTransferDepthPerIterationMeters >=
            0.0F &&
        positive(
            regolithDensityKgPerCubicMeter) &&
        positive(
            soilDensityKgPerCubicMeter) &&
        positive(
            sandDensityKgPerCubicMeter) &&
        positive(
            debrisDensityKgPerCubicMeter);
}

GpuThermalErosionPage::GpuThermalErosionPage(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const u32 maxResolution,
    const u32 maxGeologyEntries)
    : maxResolution_(maxResolution),
      maxGeologyEntries_(
          maxGeologyEntries)
{
    const u64 maxCells =
        CheckedCellCount(
            maxResolution_);

    if (maxGeologyEntries_ == 0U)
    {
        throw std::invalid_argument(
            "Orbit M12 GPU thermal solver requires geology capacity.");
    }

    computeTransferPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM12ComputeTransferShader,
            15U,
            3U,
            3U);

    applyTransferPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM12ApplyTransferShader,
            1U,
            1U,
            2U);

    snapshotPipeline_ =
        CompilePipeline(
            device,
            shaderCompiler,
            detail::
                kM12SnapshotMaterialShader,
            1U,
            1U,
            3U);

    transfers_ =
        device.CreateBuffer({
            .sizeBytes =
                maxCells *
                kTransferBytesPerCell,
            .usage =
                rhi::BufferUsage::Structured,
            .memory =
                rhi::MemoryUsage::GpuOnly,
            .initialState =
                rhi::ResourceState::UnorderedAccess
        });

    zeroProtection_ =
        device.CreateBuffer({
            .sizeBytes =
                maxCells *
                kProtectionBytesPerCell,
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

    materialSnapshot_ =
        device.CreateBuffer({
            .sizeBytes =
                maxCells *
                kMaterialSnapshotBytesPerCell,
            .usage =
                rhi::BufferUsage::Structured,
            .memory =
                rhi::MemoryUsage::GpuOnly,
            .initialState =
                rhi::ResourceState::UnorderedAccess
        });

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

GpuThermalErosionPage::~GpuThermalErosionPage() = default;

void GpuThermalErosionPage::UploadGeologyTable(
    const terrain_geology::GeologicalMaterialGpuTable& table)
{
    if (table.materials.empty() ||
        table.materials.size() !=
            table.rockTypes.size() ||
        table.materials.size() >
            maxGeologyEntries_)
    {
        throw std::invalid_argument(
            "Orbit M12 geology table is empty, inconsistent, or exceeds "
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

void GpuThermalErosionPage::DispatchSteps(
    rhi::CommandList& commandList,
    const u32 resolution,
    const f32 spacingMeters,
    const u32 stepCount,
    const GpuThermalErosionConfig& config,
    GpuMaterialColumnResources& materialColumn,
    rhi::Buffer* protection)
{
    if (!config.IsValid() ||
        resolution == 0U ||
        resolution >
            maxResolution_ ||
        materialColumn.Resolution() !=
            resolution ||
        !std::isfinite(
            spacingMeters) ||
        spacingMeters <= 0.0F)
    {
        throw std::invalid_argument(
            "Orbit M12 GPU thermal dispatch is invalid.");
    }

    if (!geologyUploaded_)
    {
        throw std::logic_error(
            "Orbit M12 GPU thermal dispatch requires an uploaded M02 "
            "geology table.");
    }

    if (stepCount == 0U)
    {
        activeResolution_ =
            resolution;
        return;
    }

    rhi::Buffer& protectionBuffer =
        protection != nullptr
            ? *protection
            : *zeroProtection_;

    const u64 protectionBytes =
        static_cast<u64>(
            resolution) *
        resolution *
        kProtectionBytesPerCell;

    RequireBufferSize(
        protectionBuffer,
        protectionBytes,
        "protection");

    const u32 groupCount =
        (resolution +
         kThreadGroupSize -
         1U) /
        kThreadGroupSize;

    for (u32 step = 0U;
         step < stepCount;
         ++step)
    {
        commandList.SetComputePipeline(
            *computeTransferPipeline_);

        commandList.SetComputeBuffer(
            0U,
            protectionBuffer);

        commandList.SetComputeBuffer(
            1U,
            *geologyTable_);

        commandList.SetComputeBuffer(
            2U,
            *transfers_);

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

        std::array<u32, 15>
            constants{};

        constants[0] =
            resolution;

        const std::array<f32, 14>
            values{
                spacingMeters,
                config.
                    sandReposeDegrees,
                config.
                    debrisReposeDegrees,
                config.
                    regolithReposeDegrees,
                config.
                    soilReposeDegrees,
                config.
                    minimumBedrockFailureDegrees,
                config.
                    bedrockFailureAngleRangeDegrees,
                config.
                    bedrockFractureRate,
                config.
                    relaxation,
                config.
                    maximumTransferDepthPerIterationMeters,
                config.
                    regolithDensityKgPerCubicMeter,
                config.
                    soilDensityKgPerCubicMeter,
                config.
                    sandDensityKgPerCubicMeter,
                config.
                    debrisDensityKgPerCubicMeter
            };

        for (u32 i = 0U;
             i < values.size();
             ++i)
        {
            constants[i + 1U] =
                std::bit_cast<u32>(
                    values[i]);
        }

        commandList.SetComputeConstants(
            constants);

        commandList.Dispatch(
            groupCount,
            groupCount,
            1U);

        commandList.UavBarrier(
            *transfers_);

        commandList.SetComputePipeline(
            *applyTransferPipeline_);

        commandList.SetComputeBuffer(
            0U,
            *transfers_);

        commandList.SetComputeStorageTexture(
            0U,
            materialColumn.
                BedrockHeight());

        commandList.SetComputeStorageTexture(
            1U,
            materialColumn.
                LooseMaterials());

        const std::array<u32, 1>
            applyConstants{
                resolution
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
    }

    activeResolution_ =
        resolution;

    readbackRecorded_ =
        false;
}

void GpuThermalErosionPage::RecordMaterialReadback(
    rhi::CommandList& commandList,
    const u32 resolution,
    GpuMaterialColumnResources& materialColumn)
{
    if (resolution == 0U ||
        resolution !=
            activeResolution_ ||
        materialColumn.Resolution() !=
            resolution)
    {
        throw std::invalid_argument(
            "Orbit M12 material readback does not match the active GPU page.");
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

    readbackRecorded_ =
        true;
}

void GpuThermalErosionPage::ApplyMaterialReadbackToCpu(
    terrain_material_column::MaterialColumnPage& page)
{
    if (!readbackRecorded_ ||
        page.Resolution() !=
            activeResolution_)
    {
        throw std::logic_error(
            "Orbit M12 CPU material sync requires a completed matching "
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

    readbackRecorded_ =
        false;
}

rhi::Buffer&
GpuThermalErosionPage::TransferProposals() noexcept
{
    return *transfers_;
}
} // namespace orbit::terrain_gpu
