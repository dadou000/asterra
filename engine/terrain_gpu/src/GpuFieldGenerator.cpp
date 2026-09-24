#include <orbit/terrain_gpu/GpuFieldGenerator.hpp>

#include "FieldGenerationCompute.hpp"

#include <orbit/terrain/GlobalTerrainFields.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace orbit::terrain_gpu
{
namespace
{
// Must match FieldGenerationCompute.hpp's kParam* indices exactly.
constexpr u32 kParamCount = 51;

constexpr u32 kPlateStrideFloats = 9;
constexpr u32 kHotspotStrideFloats = 34;
constexpr u32 kCraterStrideFloats = 7;
constexpr u32 kPushConstantDwords = 40;

[[nodiscard]] std::array<u32, kParamCount> BuildParamsBuffer(
    const world::PlanetDefinition& planet,
    const terrain::AnalyticTerrainDesc& desc,
    const f32 warpFrequency,
    const f32 warpFootprintScale,
    const f32 mountainNormalization,
    const u64 topSeed,
    const u64 globalSeed)
{
    std::array<u32, kParamCount> result{};

    const auto storeFloat = [&](const u32 index, const f64 value)
    {
        result[index] = std::bit_cast<u32>(static_cast<f32>(value));
    };
    const auto storeUint = [&](const u32 index, const u32 value)
    {
        result[index] = value;
    };
    const auto storeU64 = [&](const u32 loIndex, const u32 hiIndex, const u64 value)
    {
        result[loIndex] = static_cast<u32>(value);
        result[hiIndex] = static_cast<u32>(value >> 32);
    };

    const auto& global = desc.global;
    const auto& tectonic = global.tectonic;
    const auto& mountains = desc.mountains;

    storeFloat(0, planet.radiusMeters);
    storeFloat(1, global.seaLevelMeters);
    storeFloat(2, desc.maximumElevationAboveSeaLevelMeters);
    storeFloat(3, desc.macroAmplitudeMeters);
    storeFloat(4, desc.macroWavelengthMeters);
    storeFloat(5, desc.detailAmplitudeMeters);
    storeFloat(6, desc.detailWavelengthMeters);
    storeUint(7, desc.detailOctaves);
    storeFloat(8, mountains.reliefMeters);
    storeFloat(9, mountains.wavelengthMeters);
    storeUint(10, mountains.octaves);
    storeFloat(11, mountains.warpWavelengthMeters);
    storeFloat(12, mountains.warpAmplitudeMeters);
    storeFloat(13, warpFrequency);
    storeFloat(14, warpFootprintScale);
    storeFloat(15, mountainNormalization);
    storeFloat(16, global.continentalAmplitudeMeters);
    storeFloat(17, global.continentalWavelengthMeters);
    storeFloat(18, global.continentalBiasMeters);
    storeFloat(19, tectonic.tectonicContinentInfluence);
    storeFloat(20, global.mountainAmplitudeMeters);
    storeFloat(21, global.mountainWavelengthMeters);
    storeFloat(22, global.climateWavelengthMeters);
    storeFloat(23, global.equatorTemperatureC);
    storeFloat(24, global.poleTemperatureC);
    storeFloat(25, global.temperatureVariationC);
    storeFloat(26, global.lapseRateCPerKilometer);
    storeFloat(27, tectonic.boundaryWidthDot);
    storeFloat(28, tectonic.convergenceReferenceSpeed);
    storeFloat(29, tectonic.oceanicConvergenceScale);
    storeFloat(30, tectonic.convergenceUpliftMeters);
    storeUint(31, tectonic.plateCount);
    storeUint(32, tectonic.hotspotCount);
    storeUint(33, tectonic.hotspotAgeSteps);
    storeFloat(34, tectonic.rainShadowStrength);
    storeFloat(35, tectonic.rainShadowStepMeters);
    storeFloat(36, tectonic.rainShadowStepGrowth);
    storeFloat(37, tectonic.rainShadowThresholdMeters);
    storeFloat(38, tectonic.rainShadowRangeMeters);
    storeUint(39, tectonic.rainShadowSteps);
    storeFloat(40, tectonic.windBandTransitionDegrees);
    storeU64(41, 42, topSeed);
    storeU64(43, 44, globalSeed);
    storeUint(45, desc.craters.enabled ? desc.craters.count : 0U);
    storeFloat(46, desc.craters.complexTransitionRadiusMeters);
    storeFloat(47, desc.craters.maximumEjectaExtentRadii);
    storeFloat(48, desc.craters.minimumRadiusMeters);
    storeFloat(49, desc.craters.maximumRadiusMeters);
    storeFloat(50, desc.craters.cumulativeExponent);

    return result;
}

[[nodiscard]] std::vector<f32> BuildCratersBuffer(
    const std::vector<terrain::GpuProceduralCrater>& craters)
{
    std::vector<f32> result(
        craters.size() * kCraterStrideFloats, 0.0F);
    for (std::size_t i = 0; i < craters.size(); ++i)
    {
        const auto& crater = craters[i];
        f32* out = result.data() + i * kCraterStrideFloats;
        out[0] = static_cast<f32>(crater.centerDirection.x);
        out[1] = static_cast<f32>(crater.centerDirection.y);
        out[2] = static_cast<f32>(crater.centerDirection.z);
        out[3] = static_cast<f32>(crater.radiusMeters);
        out[4] = static_cast<f32>(crater.degradation);
        out[5] = static_cast<f32>(crater.rimIrregularityPhase);
        out[6] = static_cast<f32>(crater.boundingCosine);
    }
    return result;
}

[[nodiscard]] std::vector<f32> BuildPlatesBuffer(
    const std::vector<terrain::GpuTectonicPlate>& plates)
{
    std::vector<f32> result(
        static_cast<std::size_t>(plates.size()) * kPlateStrideFloats, 0.0F);

    for (std::size_t i = 0; i < plates.size(); ++i)
    {
        const terrain::GpuTectonicPlate& plate = plates[i];
        f32* out = result.data() + i * kPlateStrideFloats;

        out[0] = static_cast<f32>(plate.seedDirection.x);
        out[1] = static_cast<f32>(plate.seedDirection.y);
        out[2] = static_cast<f32>(plate.seedDirection.z);
        out[3] = static_cast<f32>(plate.continentalBiasMeters);
        out[4] = static_cast<f32>(plate.eulerVector.x);
        out[5] = static_cast<f32>(plate.eulerVector.y);
        out[6] = static_cast<f32>(plate.eulerVector.z);
        out[7] = static_cast<f32>(plate.sizeBiasDot);
        out[8] = plate.isContinental ? 1.0F : 0.0F;
    }

    return result;
}

[[nodiscard]] std::vector<f32> BuildHotspotsBuffer(
    const std::vector<terrain::GpuTectonicHotspot>& hotspots)
{
    std::vector<f32> result(
        static_cast<std::size_t>(hotspots.size()) * kHotspotStrideFloats, 0.0F);

    for (std::size_t h = 0; h < hotspots.size(); ++h)
    {
        const terrain::GpuTectonicHotspot& hotspot = hotspots[h];
        f32* out = result.data() + h * kHotspotStrideFloats;

        out[0] = static_cast<f32>(hotspot.mantlePosition.x);
        out[1] = static_cast<f32>(hotspot.mantlePosition.y);
        out[2] = static_cast<f32>(hotspot.mantlePosition.z);
        out[3] = static_cast<f32>(hotspot.boundingCosine);

        for (std::size_t k = 0; k < hotspot.chainPoint.size(); ++k)
        {
            f32* step = out + 4 + k * 5;
            step[0] = static_cast<f32>(hotspot.chainPoint[k].x);
            step[1] = static_cast<f32>(hotspot.chainPoint[k].y);
            step[2] = static_cast<f32>(hotspot.chainPoint[k].z);
            step[3] = static_cast<f32>(hotspot.chainAmplitude[k]);
            step[4] = static_cast<f32>(hotspot.chainChordRadius[k]);
        }
    }

    return result;
}

[[nodiscard]] std::unique_ptr<rhi::Buffer> CreateStaticBuffer(
    rhi::Device& device,
    const void* data,
    const u64 sizeBytes)
{
    // Small (well under a page), read very infrequently relative to the
    // output buffer's writes, and never written again after this call --
    // a HostVisible buffer bound directly as a compute SRV is simpler
    // and just as correct as a GpuOnly buffer plus a one-time staging
    // upload (which would need a throwaway command list/queue/fence in
    // this constructor purely to move a few hundred bytes), and this
    // codebase's other HostVisible buffers are already used the same
    // way: Map, memcpy, Unmap, then immediately readable by the GPU with
    // no separate flush (VMA's host-visible allocations here are
    // effectively coherent).
    const u64 allocateSizeBytes = std::max<u64>(sizeBytes, 4);

    auto buffer = device.CreateBuffer({
        .sizeBytes = allocateSizeBytes,
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource
    });

    if (sizeBytes > 0)
    {
        std::byte* mapped = buffer->Map();
        std::memcpy(mapped, data, sizeBytes);
        buffer->Unmap();
    }

    return buffer;
}
} // namespace

GpuFieldGenerator::GpuFieldGenerator(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const world::PlanetDefinition planet,
    const terrain::AnalyticTerrainSource& source)
    : planet_(planet)
{
    const shader::Binary compute = shaderCompiler.Compile({
        .source = detail::kFieldGenerationComputeShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });

    if (compute.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile the field generation compute "
            "shader.");
    }

    pipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = compute.bytecode.data(),
            .size = compute.bytecode.size()
        },
        .pushConstantDwords = kPushConstantDwords,
        .shaderResourceBuffers = 5
    });

    const terrain::AnalyticTerrainDesc& desc = source.Description();
    const auto& mountains = desc.mountains;

    const f64 warpFrequency =
        planet.radiusMeters / mountains.warpWavelengthMeters;
    const f64 warpFootprintScale =
        1.0 +
        11.25 * mountains.warpAmplitudeMeters / mountains.warpWavelengthMeters;

    // Geometric series sum(1.0 * 0.5^i, i=0..octaves-1) = 2*(1-0.5^octaves)
    // -- matches AnalyticTerrainSource.cpp's `prepare` lambda's
    // amplitudeSum for the mountain bands (initial amplitude 1.0,
    // halving each octave), clamped the same way.
    const f64 mountainNormalization = std::max(
        2.0 * (1.0 - std::pow(0.5, static_cast<f64>(mountains.octaves))),
        1.0);

    // Two distinct seeds, matching the CPU hash's own two-level
    // resolution exactly (see FieldGenerationCompute.hpp's
    // kParamTopSeedLo/kParamGlobalSeedLo comment): the top-level recipe
    // seed verbatim, and GlobalTerrainFields' already-resolved seed
    // (source.GlobalFields().Description().seed already has
    // GlobalTerrainFieldDesc's "derive from the top-level seed when
    // zero" default folded in -- unlike `desc.global.seed`, which this
    // AnalyticTerrainSource's own Description() still reports as
    // whatever the caller originally passed, e.g. 0).
    const u64 topSeed = desc.seed;
    const u64 globalSeed = source.GlobalFields().Description().seed;

    const auto params = BuildParamsBuffer(
        planet, desc,
        static_cast<f32>(warpFrequency),
        static_cast<f32>(warpFootprintScale),
        static_cast<f32>(mountainNormalization),
        topSeed,
        globalSeed);

    const std::vector<terrain::GpuTectonicPlate> plates =
        source.GlobalFields().TectonicPlatesForGpu();
    const std::vector<terrain::GpuTectonicHotspot> hotspots =
        source.GlobalFields().TectonicHotspotsForGpu();

    const std::vector<f32> plateFloats = BuildPlatesBuffer(plates);
    const std::vector<f32> hotspotFloats = BuildHotspotsBuffer(hotspots);
    const std::vector<f32> craterFloats =
        BuildCratersBuffer(source.CratersForGpu());

    paramsBuffer_ = CreateStaticBuffer(
        device, params.data(), params.size() * sizeof(u32));
    platesBuffer_ = CreateStaticBuffer(
        device, plateFloats.data(), plateFloats.size() * sizeof(f32));
    hotspotsBuffer_ = CreateStaticBuffer(
        device, hotspotFloats.data(), hotspotFloats.size() * sizeof(f32));
    cratersBuffer_ = CreateStaticBuffer(
        device, craterFloats.data(), craterFloats.size() * sizeof(f32));
}

GpuFieldGenerator::~GpuFieldGenerator() = default;

void GpuFieldGenerator::Dispatch(
    rhi::CommandList& commandList,
    const GpuFieldRequest& request,
    rhi::Buffer& outputSamples) const
{
    const auto toFloat3 = [](const math::Double3& v) -> std::array<f32, 3>
    {
        return {
            static_cast<f32>(v.x),
            static_cast<f32>(v.y),
            static_cast<f32>(v.z)
        };
    };

    std::array<u32, kPushConstantDwords> pushConstants{};
    const auto storeFloat = [&](const u32 index, const f64 value)
    {
        pushConstants[index] = std::bit_cast<u32>(static_cast<f32>(value));
    };
    const auto storeUint = [&](const u32 index, const u32 value)
    {
        pushConstants[index] = value;
    };
    const auto storeFloat3 = [&](const u32 index, const math::Double3& v)
    {
        const auto f = toFloat3(v);
        pushConstants[index + 0] = std::bit_cast<u32>(f[0]);
        pushConstants[index + 1] = std::bit_cast<u32>(f[1]);
        pushConstants[index + 2] = std::bit_cast<u32>(f[2]);
    };

    storeUint(0, request.resolution);
    storeFloat(1, request.spacingMeters);
    storeFloat(2, request.footprintMeters);
    storeUint(3, request.morphToCoarser ? 1U : 0U);
    storeFloat(4, request.morphStartHalfExtentMeters);
    storeFloat(5, request.morphEndHalfExtentMeters);
    storeFloat(6, request.coarseSpacingMeters);
    storeFloat(7, request.coarseFootprintMeters);
    storeUint(8, request.originX);
    storeUint(9, request.originY);
    storeFloat(10, request.fineNormalFootprintMeters);
    storeFloat(11, request.fineNormalEpsilonMeters);
    storeUint(12, request.region.x);
    storeUint(13, request.region.y);
    storeUint(14, request.region.width);
    storeUint(15, request.region.height);
    storeFloat3(16, request.surfaceFrame.up);
    storeFloat(19, request.centerOffsetMeters.x);
    storeFloat3(20, request.surfaceFrame.east);
    storeFloat(23, request.centerOffsetMeters.y);
    storeFloat3(24, request.surfaceFrame.north);
    storeFloat3(28, request.coarseSurfaceFrame.up);
    storeFloat3(32, request.coarseSurfaceFrame.east);
    storeFloat3(36, request.coarseSurfaceFrame.north);

    commandList.SetComputePipeline(*pipeline_);
    commandList.SetComputeBuffer(0, *paramsBuffer_);
    commandList.SetComputeBuffer(1, *platesBuffer_);
    commandList.SetComputeBuffer(2, *hotspotsBuffer_);
    commandList.SetComputeBuffer(3, outputSamples);
    commandList.SetComputeBuffer(4, *cratersBuffer_);
    commandList.SetComputeConstants(pushConstants);

    constexpr u32 kThreadGroupSize = 8;
    const u32 groupCountX =
        (request.region.width + kThreadGroupSize - 1) / kThreadGroupSize;
    const u32 groupCountY =
        (request.region.height + kThreadGroupSize - 1) / kThreadGroupSize;

    commandList.Dispatch(std::max(groupCountX, 1U), std::max(groupCountY, 1U), 1);
}
} // namespace orbit::terrain_gpu
