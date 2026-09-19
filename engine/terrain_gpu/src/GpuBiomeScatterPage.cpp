#include <orbit/terrain_gpu/GpuBiomeScatterPage.hpp>

#include "M22ScatterCompute.hpp"

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
constexpr u64 kFieldBytesPerCell = 32U;
constexpr u64 kInstanceBytesPerCell = 48U;

struct GpuScatterFieldSample
{
    f32 biomeWeight{0.0F};
    f32 slopeDegrees{0.0F};
    f32 soilDepthMeters{0.0F};
    f32 moisture{0.0F};
    f32 exclusionMask{0.0F};
    f32 authoredDensity{1.0F};
    u32 exposedMaterial{0U};
    u32 reserved{0U};
};

static_assert(
    sizeof(GpuScatterFieldSample) ==
    kFieldBytesPerCell);

[[nodiscard]] u64 CheckedCellCount(
    const u32 resolution)
{
    if (resolution == 0U)
    {
        throw std::invalid_argument(
            "Orbit M22 GPU scatter requires non-zero resolution.");
    }

    const u64 count =
        static_cast<u64>(resolution) *
        resolution;

    if (count >
        std::numeric_limits<u32>::max())
    {
        throw std::invalid_argument(
            "Orbit M22 GPU scatter exceeds 32-bit cell addressing.");
    }

    return count;
}

[[nodiscard]] std::unique_ptr<
    rhi::ComputePipeline>
CompileScatterPipeline(
    rhi::Device& device,
    const shader::Compiler& compiler)
{
    const shader::Binary binary =
        compiler.Compile({
            .source =
                detail::
                    kM22BiomeScatterShader,
            .entryPoint = "main",
            .stage =
                shader::Stage::Compute,
            .debug = false
        });

    if (binary.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile the M22 biome scatter compute shader.");
    }

    return
        device.CreateComputePipeline({
            .computeShader = {
                .data =
                    binary.bytecode.data(),
                .size =
                    binary.bytecode.size()
            },
            .pushConstantDwords = 19U,
            .shaderResourceBuffers = 2U
        });
}

[[nodiscard]] u64 AssembleU64(
    const u32 low,
    const u32 high) noexcept
{
    return
        static_cast<u64>(low) |
        (static_cast<u64>(high) <<
         32U);
}
} // namespace

GpuBiomeScatterPage::GpuBiomeScatterPage(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const u32 maxResolution)
    : maxResolution_(maxResolution)
{
    const u64 maxCells =
        CheckedCellCount(
            maxResolution_);

    scatterPipeline_ =
        CompileScatterPipeline(
            device,
            shaderCompiler);

    fieldInput_ =
        device.CreateBuffer({
            .sizeBytes =
                maxCells *
                kFieldBytesPerCell,
            .usage =
                rhi::BufferUsage::Structured,
            .memory =
                rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::UnorderedAccess
        });

    instanceSlots_ =
        device.CreateBuffer({
            .sizeBytes =
                maxCells *
                kInstanceBytesPerCell,
            .usage =
                rhi::BufferUsage::Structured,
            .memory =
                rhi::MemoryUsage::GpuOnly,
            .initialState =
                rhi::ResourceState::UnorderedAccess
        });

    instanceReadback_ =
        device.CreateBuffer({
            .sizeBytes =
                maxCells *
                kInstanceBytesPerCell,
            .usage =
                rhi::BufferUsage::Generic,
            .memory =
                rhi::MemoryUsage::HostReadback,
            .initialState =
                rhi::ResourceState::CopyDestination
        });
}

GpuBiomeScatterPage::~GpuBiomeScatterPage() =
    default;

void GpuBiomeScatterPage::Dispatch(
    rhi::CommandList& commandList,
    const terrain_scatter::ScatterPageRequest& request,
    const std::span<
        const terrain_scatter::ScatterCellInput> cells)
{
    if (!request.IsValid() ||
        request.gridResolution >
            maxResolution_)
    {
        throw std::invalid_argument(
            "Orbit M22 GPU scatter request is invalid or exceeds fixed capacity.");
    }

    const u64 cellCount =
        CheckedCellCount(
            request.gridResolution);

    if (cells.size() !=
        cellCount)
    {
        throw std::invalid_argument(
            "Orbit M22 GPU scatter field does not match planting-grid resolution.");
    }

    auto* mapped =
        fieldInput_->Map();

    for (u64 index = 0U;
         index < cellCount;
         ++index)
    {
        const auto& cell =
            cells[
                static_cast<
                    std::size_t>(
                        index)];

        if (!cell.IsValid())
        {
            fieldInput_->Unmap();

            throw std::invalid_argument(
                "Orbit M22 GPU scatter input contains invalid physical fields.");
        }

        const GpuScatterFieldSample packed{
            .biomeWeight =
                cell.biomeWeight,
            .slopeDegrees =
                cell.slopeDegrees,
            .soilDepthMeters =
                cell.soilDepthMeters,
            .moisture =
                cell.moisture,
            .exclusionMask =
                cell.exclusionMask,
            .authoredDensity =
                cell.authoredDensity,
            .exposedMaterial =
                static_cast<u32>(
                    cell.exposedMaterial)
        };

        std::memcpy(
            mapped +
                index *
                    kFieldBytesPerCell,
            &packed,
            sizeof(packed));
    }

    fieldInput_->Unmap();

    commandList.SetComputePipeline(
        *scatterPipeline_);

    commandList.SetComputeBuffer(
        0U,
        *fieldInput_);

    commandList.SetComputeBuffer(
        1U,
        *instanceSlots_);

    std::array<u32, 19>
        constants{};

    constants[0] =
        request.gridResolution;

    constants[1] =
        std::bit_cast<u32>(
            request.cellSizeMeters);

    constants[2] =
        std::bit_cast<u32>(
            request.rule.
                minimumSpacingMeters);

    constants[3] =
        std::bit_cast<u32>(
            request.rule.
                densityPerSquareMeter);

    constants[4] =
        std::bit_cast<u32>(
            request.
                biomeDensityMultiplier);

    constants[5] =
        static_cast<u32>(
            request.rule.kind);

    constants[6] =
        terrain_scatter::
            ScatterPageHash(
                request.identity);

    constants[7] =
        terrain_scatter::
            ScatterRuleHash(
                request.rule);

    constants[8] =
        static_cast<u32>(
            request.rule.
                compatibleExposed);

    constants[9] =
        request.rule.
            requiresSoil
            ? 1U
            : 0U;

    constants[10] =
        std::bit_cast<u32>(
            request.rule.
                minimumSoilDepthMeters);

    constants[11] =
        std::bit_cast<u32>(
            request.rule.
                minimumSlopeDegrees);

    constants[12] =
        std::bit_cast<u32>(
            request.rule.
                maximumSlopeDegrees);

    constants[13] =
        std::bit_cast<u32>(
            request.rule.
                slopeFalloffDegrees);

    constants[14] =
        std::bit_cast<u32>(
            request.rule.
                minimumMoisture);

    constants[15] =
        std::bit_cast<u32>(
            request.rule.
                maximumMoisture);

    constants[16] =
        std::bit_cast<u32>(
            request.rule.
                moistureFalloff);

    constants[17] =
        std::bit_cast<u32>(
            request.rule.
                minimumScale);

    constants[18] =
        std::bit_cast<u32>(
            request.rule.
                maximumScale);

    commandList.SetComputeConstants(
        constants);

    const u32 groupCount =
        (request.gridResolution +
         kThreadGroupSize -
         1U) /
        kThreadGroupSize;

    commandList.Dispatch(
        groupCount,
        groupCount,
        1U);

    commandList.UavBarrier(
        *instanceSlots_);

    activeResolution_ =
        request.gridResolution;

    dispatched_ = true;
    readbackRecorded_ = false;
}

void GpuBiomeScatterPage::RecordReadback(
    rhi::CommandList& commandList)
{
    if (!dispatched_ ||
        activeResolution_ == 0U)
    {
        throw std::logic_error(
            "Orbit M22 GPU scatter readback requires a completed dispatch.");
    }

    const u64 bytes =
        static_cast<u64>(
            activeResolution_) *
        activeResolution_ *
        kInstanceBytesPerCell;

    commandList.Transition(
        *instanceSlots_,
        rhi::ResourceState::UnorderedAccess,
        rhi::ResourceState::CopySource);

    commandList.CopyBuffer(
        *instanceSlots_,
        0U,
        *instanceReadback_,
        0U,
        bytes);

    commandList.Transition(
        *instanceSlots_,
        rhi::ResourceState::CopySource,
        rhi::ResourceState::UnorderedAccess);

    readbackRecorded_ = true;
}

std::vector<
    terrain_scatter::DerivedScatterInstance>
GpuBiomeScatterPage::ApplyReadback()
{
    if (!readbackRecorded_ ||
        activeResolution_ == 0U)
    {
        throw std::logic_error(
            "Orbit M22 GPU scatter CPU readback requires a completed submission fence.");
    }

    const u64 cellCount =
        static_cast<u64>(
            activeResolution_) *
        activeResolution_;

    const std::byte* mapped =
        instanceReadback_->Map();

    std::vector<
        terrain_scatter::
            DerivedScatterInstance>
        result;

    result.reserve(
        static_cast<
            std::size_t>(
                cellCount /
                2U));

    for (u64 index = 0U;
         index < cellCount;
         ++index)
    {
        std::array<u32, 12>
            words{};

        std::memcpy(
            words.data(),
            mapped +
                index *
                    kInstanceBytesPerCell,
            kInstanceBytesPerCell);

        if (words[9] == 0U)
        {
            continue;
        }

        terrain_scatter::
            DerivedScatterInstance
            instance{
                .id = {
                    .high =
                        AssembleU64(
                            words[4],
                            words[5]),
                    .low =
                        AssembleU64(
                            words[6],
                            words[7])
                },
                .kind =
                    static_cast<
                        terrain_biome::
                            BiomeScatterKind>(
                                words[8]),
                .localOffsetMeters = {
                    static_cast<f64>(
                        std::bit_cast<f32>(
                            words[0])),
                    static_cast<f64>(
                        std::bit_cast<f32>(
                            words[1]))
                },
                .yawRadians =
                    std::bit_cast<f32>(
                        words[2]),
                .uniformScale =
                    std::bit_cast<f32>(
                        words[3]),
                .cellX =
                    words[10],
                .cellY =
                    words[11]
            };

        if (!instance.IsValid())
        {
            instanceReadback_->Unmap();

            throw std::logic_error(
                "Orbit M22 GPU scatter readback contained an invalid active slot.");
        }

        result.push_back(
            instance);
    }

    instanceReadback_->Unmap();

    readbackRecorded_ = false;

    return result;
}

rhi::Buffer&
GpuBiomeScatterPage::InstanceSlots() noexcept
{
    return *instanceSlots_;
}

u32 GpuBiomeScatterPage::ActiveResolution()
    const noexcept
{
    return activeResolution_;
}
} // namespace orbit::terrain_gpu
