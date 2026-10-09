#include <orbit/terrain_gpu/GpuGeologyCompiler.hpp>

#include "GeologyCompute.hpp"

#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Fence.hpp>
#include <orbit/rhi/Query.hpp>
#include <orbit/rhi/Queue.hpp>
#include <orbit/terrain/BakedGeology.hpp>
#include <orbit/terrain_impacts/ImpactField.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orbit::terrain_gpu
{
namespace
{
constexpr u32 kTileSize = 32U;
constexpr u32 kMaximumTilesPerSubmission = 24U;
constexpr u64 kMaximumSubmissionPayloadBytes = 16U * 1024U * 1024U;
constexpr u32 kThreadGroupSize = 8U;
constexpr u32 kPushConstantDwords = 25U;

struct alignas(16) EventPacket
{
    std::array<u32, 4> meta{};
    std::array<std::array<f32, 4>, 8> data{};
};
static_assert(sizeof(EventPacket) == 144U);

struct alignas(16) SegmentPacket
{
    std::array<f32, 4> start{};
    std::array<f32, 4> end{};
    std::array<f32, 4> midpoint{};
};
static_assert(sizeof(SegmentPacket) == 48U);

struct GpuOutput
{
    f32 impactHeight;
    f32 iceHeight;
    f32 excavationDepth;
    f32 ejectaThickness;
    f32 debrisField;
    f32 rayField;
    f32 meltThickness;
    f32 brecciaField;
    f32 resurfacedMaterialFraction;
    f32 resurfacingThickness;
    f32 microImpactRoughness;
    f32 microImpactCoverage;
    f32 excavationCoverage;
    f32 formationAgeYears;
    f32 exposureAgeYears;
    u32 formationAgeLow;
    u32 formationAgeHigh;
    u32 exposureAgeLow;
    u32 exposureAgeHigh;
    u32 affectingImpacts;
    f32 iceDamage;
    f32 fractureCoverage;
    u32 nearbySegments;
};
static_assert(sizeof(GpuOutput) == 92U);

struct TileWork
{
    u32 face{0U};
    i32 x{0};
    i32 y{0};
    u32 width{0U};
    u32 height{0U};
    std::vector<EventPacket> events;
    std::vector<std::array<f32, 4>> flowPoints;
    std::vector<SegmentPacket> segments;
    std::array<u32, kPushConstantDwords> constants{};
    std::unique_ptr<rhi::Buffer> eventBuffer;
    std::unique_ptr<rhi::Buffer> flowBuffer;
    std::unique_ptr<rhi::Buffer> segmentBuffer;
    std::unique_ptr<rhi::Buffer> outputBuffer;
    std::unique_ptr<rhi::Buffer> readbackBuffer;
};

struct TileRegion
{
    u32 face{0U};
    i32 x{0};
    i32 y{0};
    u32 width{0U};
    u32 height{0U};
};

[[nodiscard]] u64 Mix64(u64 value) noexcept
{
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] f64 UnitFloat(const u64 value) noexcept
{
    return static_cast<f64>(value >> 11U) / static_cast<f64>(1ULL << 53U);
}

[[nodiscard]] std::array<f32, 3> ToFloat3(const math::Double3& value) noexcept
{
    return {static_cast<f32>(value.x), static_cast<f32>(value.y),
        static_cast<f32>(value.z)};
}

[[nodiscard]] std::unique_ptr<rhi::Buffer> MakeInputBuffer(
    rhi::Device& device,
    const void* data,
    const std::size_t bytes,
    const std::size_t minimumBytes)
{
    auto buffer = device.CreateBuffer({
        .sizeBytes = std::max<u64>(static_cast<u64>(bytes),
            static_cast<u64>(minimumBytes)),
        .usage = rhi::BufferUsage::Structured,
        .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    std::byte* mapped = buffer->Map();
    std::memset(mapped, 0, static_cast<std::size_t>(buffer->SizeBytes()));
    if (bytes > 0U) std::memcpy(mapped, data, bytes);
    buffer->Unmap();
    return buffer;
}

[[nodiscard]] std::unique_ptr<rhi::Buffer> MakeOutputBuffer(
    rhi::Device& device,
    const u64 bytes,
    const rhi::MemoryUsage memory,
    const rhi::ResourceState initialState)
{
    return device.CreateBuffer({
        .sizeBytes = std::max<u64>(bytes, 16U),
        .usage = rhi::BufferUsage::Structured,
        .memory = memory,
        .initialState = initialState});
}

void SetFloat(std::array<u32, kPushConstantDwords>& constants,
    const u32 index, const f64 value)
{
    constants[index] = std::bit_cast<u32>(static_cast<f32>(value));
}

void PackImpact(
    EventPacket& packet,
    const terrain_impacts::ImpactRecord& impact,
    const terrain_impacts::PreparedImpactGeometry& prepared)
{
    packet.meta = {
        0U,
        static_cast<u32>(impact.ageOrder),
        static_cast<u32>(impact.ageOrder >> 32U),
        impact.enabled ? static_cast<u32>(impact.profile) : 0xffffffffU};
    const auto center = ToFloat3(prepared.frame.up);
    const auto east = ToFloat3(prepared.frame.east);
    const auto north = ToFloat3(prepared.frame.north);
    packet.data[0] = {center[0], center[1], center[2], static_cast<f32>(impact.radiusMeters)};
    packet.data[1] = {east[0], east[1], east[2], static_cast<f32>(impact.rimHeightRatio)};
    packet.data[2] = {north[0], north[1], north[2], static_cast<f32>(impact.complexDepthRatio)};
    packet.data[3] = {static_cast<f32>(impact.ejectaThicknessRatio),
        static_cast<f32>(impact.ejectaExtentRadii), static_cast<f32>(impact.rayStrength),
        static_cast<f32>(impact.rayCount)};
    packet.data[4] = {static_cast<f32>(impact.degradation),
        static_cast<f32>(impact.formationAgeYears),
        static_cast<f32>(impact.impactAngleDegrees),
        static_cast<f32>(impact.impactAzimuthRadians)};
    packet.data[5] = {static_cast<f32>(impact.shapeIrregularity),
        static_cast<f32>(impact.meltFraction), static_cast<f32>(impact.brecciaFraction),
        static_cast<f32>(impact.multiringStrength)};
    packet.data[6] = {static_cast<f32>(prepared.ejectaMassBalanceScale),
        static_cast<f32>(prepared.phase), 0.0F, 0.0F};
    packet.data[7][2] = static_cast<f32>(impact.simpleDepthRatio);
}

void PackResurfacing(
    TileWork& tile,
    EventPacket& packet,
    const terrain_impacts::ResurfacingRecord& event)
{
    const u32 pathOffset = static_cast<u32>(tile.flowPoints.size());
    for (const math::Double3& direction : event.centerlineUnitDirections)
    {
        const auto point = ToFloat3(direction);
        tile.flowPoints.push_back({point[0], point[1], point[2], 0.0F});
    }
    packet.meta = {event.enabled ? static_cast<u32>(event.kind) + 1U : 0xffffffffU,
        static_cast<u32>(event.ageOrder), static_cast<u32>(event.ageOrder >> 32U),
        pathOffset};
    packet.data[6] = {0.0F, 0.0F,
        static_cast<f32>(event.centerlineUnitDirections.size()),
        static_cast<f32>(event.widthMeters)};
    packet.data[7] = {static_cast<f32>(event.thicknessMeters),
        static_cast<f32>(event.formationAgeYears), 0.0F, 0.0F};
}

[[nodiscard]] f64 TileAngularRadius(
    const u32 face,
    const i32 x,
    const i32 y,
    const u32 width,
    const u32 height,
    const u32 resolution,
    const math::Double3& center)
{
    const std::array<std::pair<i32, i32>, 4U> corners{{
        {x, y}, {x + static_cast<i32>(width) - 1, y},
        {x, y + static_cast<i32>(height) - 1},
        {x + static_cast<i32>(width) - 1, y + static_cast<i32>(height) - 1}}};
    f64 radius = 0.0;
    for (const auto& [cornerX, cornerY] : corners)
    {
        const math::Double3 corner = terrain::BakedGeologyTexelDirection(
            face, cornerX, cornerY, resolution);
        radius = std::max(radius,
            std::acos(std::clamp(math::Dot(center, corner), -1.0, 1.0)));
    }
    return radius;
}

void StoreGpuOutput(
    const GpuOutput& source,
    const std::size_t destination,
    std::vector<f32>& impactRelief,
    std::vector<f32>& iceRelief,
    std::vector<terrain::BakedGeologyProcessTexel>& process)
{
    impactRelief[destination] = source.impactHeight;
    iceRelief[destination] = source.iceHeight;
    process[destination] = {
        .excavationDepthMeters = source.excavationDepth,
        .ejectaThicknessMeters = source.ejectaThickness,
        .debrisField = source.debrisField,
        .rayField = source.rayField,
        .meltThicknessMeters = source.meltThickness,
        .brecciaField = source.brecciaField,
        .resurfacedMaterialFraction = source.resurfacedMaterialFraction,
        .resurfacingThicknessMeters = source.resurfacingThickness,
        .microImpactRoughnessMeters = source.microImpactRoughness,
        .microImpactCoverage = source.microImpactCoverage,
        .excavationCoverage = source.excavationCoverage,
        .formationAgeYears = source.formationAgeYears,
        .exposureAgeYears = source.exposureAgeYears,
        .formationAgeOrder = static_cast<u64>(source.formationAgeLow) |
            (static_cast<u64>(source.formationAgeHigh) << 32U),
        .exposureAgeOrder = static_cast<u64>(source.exposureAgeLow) |
            (static_cast<u64>(source.exposureAgeHigh) << 32U),
        .affectingImpacts = source.affectingImpacts,
        .iceDamage = source.iceDamage,
        .fractureCoverage = source.fractureCoverage,
        .nearbySegments = source.nearbySegments};
}
} // namespace

GpuGeologyCompiler::GpuGeologyCompiler(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler)
    : device_(device)
{
    const shader::Binary shader = shaderCompiler.Compile({
        .source = detail::kGeologyComputeShader,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false});
    if (shader.bytecode.empty())
        throw std::runtime_error("Orbit failed to compile the geological event compute shader.");
    pipeline_ = device_.CreateComputePipeline({
        .computeShader = {.data = shader.bytecode.data(), .size = shader.bytecode.size()},
        .pushConstantDwords = kPushConstantDwords,
        .shaderResourceBuffers = 4U});
    if (pipeline_ == nullptr)
        throw std::runtime_error("Orbit failed to create the geological event compute pipeline.");
}

GpuGeologyCompiler::~GpuGeologyCompiler() = default;

GpuGeologyCompileProduct GpuGeologyCompiler::Compile(
    const world::PlanetDefinition& planet,
    const terrain::AnalyticTerrainDesc& desc,
    const terrain::AnalyticTerrainDesc& previousDesc,
    std::shared_ptr<const terrain::BakedGeologyRasters> previous,
    const u32 resolution,
    const u64 recipeHash,
    const std::function<bool()>& isCancelled,
    const std::function<void(u64, u64)>& reportProgress) const
{
    const std::scoped_lock compileLock(compileMutex_);
    if (resolution < 2U || resolution > 4096U || recipeHash == 0U ||
        !planet.id.IsValid() || !(planet.radiusMeters > 0.0))
        throw std::invalid_argument("GPU geology compile request is invalid.");
    if (isCancelled && isCancelled()) return {};

    const terrain_impacts::IceFractureDefinition* iceDefinition = desc.iceFractures.get();
    if (iceDefinition == nullptr && desc.impactHistory != nullptr &&
        desc.impactHistory->iceFractures != nullptr)
        iceDefinition = desc.impactHistory->iceFractures.get();
    const bool hasIce = iceDefinition != nullptr && iceDefinition->enabled;
    if (desc.impactHistory == nullptr && !hasIce) return {};

    std::unique_ptr<terrain_impacts::ImpactField> impact;
    if (desc.impactHistory != nullptr)
        impact = std::make_unique<terrain_impacts::ImpactField>(planet, *desc.impactHistory);
    std::unique_ptr<terrain_impacts::IceFractureField> ice;
    if (hasIce) ice = std::make_unique<terrain_impacts::IceFractureField>(planet, *iceDefinition);

    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    const std::size_t fullRasterSamples = 6U * stride * stride;
    const bool canTryLocal = previous != nullptr && previous->HasProcessChannels() &&
        previous->Resolution() == resolution && previousDesc.impactHistory != nullptr &&
        desc.impactHistory != nullptr && previousDesc.iceFractures == desc.iceFractures &&
        previous->ReliefGutter().size() == fullRasterSamples &&
        previous->ProcessGutter().size() == fullRasterSamples;
    std::optional<std::vector<terrain_impacts::GeologicalInfluenceCap>> changedCaps;
    if (canTryLocal)
        changedCaps = terrain_impacts::ChangedAuthoredEventInfluenceCaps(
            planet, *previousDesc.impactHistory, *desc.impactHistory);
    const bool partial = changedCaps.has_value();
    std::vector<f32> impactRelief = partial
        ? previous->ReliefGutter() : std::vector<f32>(fullRasterSamples, 0.0F);
    std::vector<f32> iceRelief = partial
        ? previous->IceReliefGutter() : std::vector<f32>(fullRasterSamples, 0.0F);
    std::vector<terrain::BakedGeologyProcessTexel> process = partial
        ? previous->ProcessGutter()
        : std::vector<terrain::BakedGeologyProcessTexel>(fullRasterSamples);
    const u32 tilesAcross = (static_cast<u32>(stride) + kTileSize - 1U) / kTileSize;
    const u32 allTiles = 6U * tilesAcross * tilesAcross;
    const f64 footprint = 2.0 * planet.radiusMeters / static_cast<f64>(resolution);
    const f64 haloAngle = std::min(footprint / planet.radiusMeters,
        std::numbers::pi_v<f64>);
    std::vector<TileRegion> regions;
    regions.reserve(partial ? std::min<u32>(allTiles, 1024U) : allTiles);
    for (u32 tileIndex = 0U; tileIndex < allTiles; ++tileIndex)
    {
        TileRegion region;
        const u32 tilesPerFace = tilesAcross * tilesAcross;
        region.face = tileIndex / tilesPerFace;
        const u32 local = tileIndex % tilesPerFace;
        region.x = static_cast<i32>((local % tilesAcross) * kTileSize) - 1;
        region.y = static_cast<i32>((local / tilesAcross) * kTileSize) - 1;
        region.width = std::min(kTileSize,
            static_cast<u32>(stride) - static_cast<u32>(region.x + 1));
        region.height = std::min(kTileSize,
            static_cast<u32>(stride) - static_cast<u32>(region.y + 1));
        if (partial)
        {
            const i32 centerX = region.x + static_cast<i32>(region.width / 2U);
            const i32 centerY = region.y + static_cast<i32>(region.height / 2U);
            const math::Double3 center = terrain::BakedGeologyTexelDirection(
                region.face, centerX, centerY, resolution);
            const f64 tileRadius = TileAngularRadius(region.face, region.x, region.y,
                region.width, region.height, resolution, center);
            bool affected = false;
            for (const auto& cap : *changedCaps)
            {
                const f64 bound = std::min(tileRadius + haloAngle +
                    cap.angularRadiusRadians, std::numbers::pi_v<f64>);
                if (math::Dot(center, cap.centerDirection) >= std::cos(bound))
                {
                    affected = true;
                    break;
                }
            }
            if (!affected) continue;
        }
        regions.push_back(region);
    }
    const u32 tileCount = static_cast<u32>(regions.size());
    if (partial && tileCount == 0U)
    {
        GpuGeologyCompileProduct unchanged;
        unchanged.rasters = std::make_shared<const terrain::BakedGeologyRasters>(
            terrain::BakedGeologyRasters::Build(resolution, recipeHash,
                std::move(impactRelief), std::move(iceRelief), std::move(process)));
        return unchanged;
    }
    const f64 surfaceAge = desc.impactHistory != nullptr
        ? desc.impactHistory->surfaceAgeYears : 0.0;
    const f64 gravity = desc.impactHistory != nullptr
        ? desc.impactHistory->surfaceGravityMetersPerSecondSquared : 1.62;
    const f64 transitionRadius = desc.impactHistory != nullptr
        ? desc.impactHistory->complexTransitionRadiusMeters : 18'000.0;

    u32 microCount = 0U;
    f64 microMaximumRadius = 0.0;
    f64 microRepresentativeRadius = 0.0;
    f64 microPhase = 0.0;
    f64 microFrequency = 0.0;
    if (impact != nullptr)
    {
        microCount = impact->StatisticalMicroImpactCount();
        microMaximumRadius = impact->StatisticalMicroImpactMaximumRadiusMeters();
        if (microCount > 0U && microMaximumRadius > 0.0)
        {
            microRepresentativeRadius = std::sqrt(
                desc.impactHistory->procedural.minimumRadiusMeters * microMaximumRadius);
            const u64 seed = desc.impactHistory->seed != 0U
                ? desc.impactHistory->seed
                : Mix64(planet.generationSeed ^ 0x494D504143544D37ULL);
            microPhase = UnitFloat(Mix64(seed ^ 0x4D4943524F524553ULL)) *
                2.0 * std::numbers::pi_v<f64>;
            microFrequency = planet.radiusMeters /
                std::max(microRepresentativeRadius * 2.0, 1.0);
        }
    }

    const u64 eventRecords =
        (impact != nullptr
            ? static_cast<u64>(impact->ResolvedImpacts().size()) +
                static_cast<u64>(impact->Definition().resurfacingEvents.size())
            : 0U) +
        (ice != nullptr ? static_cast<u64>(ice->SegmentCount()) : 0U);
    const auto started = std::chrono::steady_clock::now();
    auto queue = device_.CreateQueue(rhi::QueueType::Compute);
    if (queue == nullptr)
        throw std::runtime_error("GPU geology compiler could not create a compute queue.");
    auto allocator = device_.CreateCommandAllocator(rhi::QueueType::Compute);
    if (allocator == nullptr)
        throw std::runtime_error("GPU geology compiler could not create a command allocator.");
    auto commandList = device_.CreateCommandList(*allocator);
    if (commandList == nullptr)
        throw std::runtime_error("GPU geology compiler could not create a command list.");
    auto fence = device_.CreateFence(0U);
    if (fence == nullptr)
        throw std::runtime_error("GPU geology compiler could not create a fence.");
    auto timestamps = device_.TimestampPeriodNanoseconds() > 0.0
        ? device_.CreateTimestampQueryPool(2U) : nullptr;

    u64 completedTiles = 0U;
    u64 samplesProcessed = 0U;
    u64 transferBytes = 0U;
    u64 dispatches = 0U;
    f64 fenceWaitMilliseconds = 0.0;
    f64 gpuQueueMilliseconds = 0.0;
    bool gpuTimestampAvailable = timestamps != nullptr;
    u64 fenceValue = 0U;
    for (u32 batchStart = 0U; batchStart < tileCount;)
    {
        if (isCancelled && isCancelled()) return {};
        std::vector<TileWork> batch;
        const u32 maximumBatchEnd = static_cast<u32>(std::min<u64>(
            tileCount, static_cast<u64>(batchStart) + kMaximumTilesPerSubmission));
        batch.reserve(maximumBatchEnd - batchStart);
        u64 batchPayloadBytes = 0U;
        u32 batchEnd = batchStart;
        for (u32 tileIndex = batchStart; tileIndex < maximumBatchEnd; ++tileIndex)
        {
            if (isCancelled && isCancelled()) return {};
            TileWork tile;
            const TileRegion& region = regions[tileIndex];
            tile.face = region.face;
            tile.x = region.x;
            tile.y = region.y;
            tile.width = region.width;
            tile.height = region.height;

            const i32 centerX = tile.x + static_cast<i32>(tile.width / 2U);
            const i32 centerY = tile.y + static_cast<i32>(tile.height / 2U);
            const math::Double3 center = terrain::BakedGeologyTexelDirection(
                tile.face, centerX, centerY, resolution);
            const f64 tileRadius = TileAngularRadius(tile.face, tile.x, tile.y,
                tile.width, tile.height, resolution, center);
            const f64 capRadius = std::min(tileRadius +
                2.0 * footprint / planet.radiusMeters, std::numbers::pi_v<f64>);
            terrain_impacts::ImpactQueryScratch scratch;
            std::vector<terrain_impacts::GeologicalEventReference> eventBatch;
            if (impact != nullptr)
                impact->CollectEventsIntersectingCap(
                    center, capRadius, scratch, eventBatch);
            if (impact != nullptr)
            {
                tile.events.reserve(eventBatch.size());
                for (const auto& reference : eventBatch)
                {
                    EventPacket packet{};
                    if (reference.kind == terrain_impacts::GeologicalEventKind::Impact)
                    {
                        PackImpact(packet, impact->ResolvedImpacts()[reference.index],
                            impact->PreparedImpactGeometries()[reference.index]);
                    }
                    else
                    {
                        PackResurfacing(tile, packet,
                            impact->Definition().resurfacingEvents[reference.index]);
                    }
                    tile.events.push_back(packet);
                }
            }
            if (ice != nullptr)
            {
                std::vector<std::size_t> segments;
                ice->CollectSegmentsIntersectingCap(
                    center, capRadius, scratch, segments);
                tile.segments.reserve(segments.size());
                for (const std::size_t index : segments)
                {
                    const auto& source = ice->Segments()[index];
                    SegmentPacket packet{};
                    const auto startDirection = ToFloat3(source.startDirection);
                    const auto endDirection = ToFloat3(source.endDirection);
                    const auto midpointDirection = ToFloat3(source.midpointDirection);
                    packet.start = {startDirection[0], startDirection[1], startDirection[2], 0.0F};
                    packet.end = {endDirection[0], endDirection[1], endDirection[2], 0.0F};
                    packet.midpoint = {midpointDirection[0], midpointDirection[1], midpointDirection[2], 0.0F};
                    tile.segments.push_back(packet);
                }
            }

            auto& constants = tile.constants;
            constants[0] = resolution;
            constants[1] = tile.face;
            constants[2] = std::bit_cast<u32>(tile.x);
            constants[3] = std::bit_cast<u32>(tile.y);
            constants[4] = tile.width;
            constants[5] = tile.height;
            constants[6] = static_cast<u32>(tile.events.size());
            constants[7] = static_cast<u32>(tile.segments.size());
            SetFloat(constants, 8U, planet.radiusMeters);
            SetFloat(constants, 9U, footprint);
            SetFloat(constants, 10U, surfaceAge);
            SetFloat(constants, 11U, gravity);
            SetFloat(constants, 12U, transitionRadius);
            constants[13] = desc.impactHistory != nullptr
                ? static_cast<u32>(desc.impactHistory->environment) : 0U;
            constants[14] = microCount;
            SetFloat(constants, 15U, microMaximumRadius);
            SetFloat(constants, 16U, microRepresentativeRadius);
            SetFloat(constants, 17U, microPhase);
            SetFloat(constants, 18U, microFrequency);
            constants[19] = ice != nullptr ? static_cast<u32>(ice->Definition().ageOrder) : 0U;
            constants[20] = ice != nullptr
                ? static_cast<u32>(ice->Definition().ageOrder >> 32U) : 0U;
            SetFloat(constants, 21U, ice != nullptr ? ice->Definition().formationAgeYears : 0.0);
            SetFloat(constants, 22U, ice != nullptr ? ice->Definition().widthMeters : 0.0);
            SetFloat(constants, 23U, ice != nullptr ? ice->Definition().grooveDepthMeters : 0.0);
            SetFloat(constants, 24U, ice != nullptr ? ice->Definition().ridgeHeightMeters : 0.0);

            const std::size_t outputElements =
                static_cast<std::size_t>(tile.width) * tile.height;
            tile.eventBuffer = MakeInputBuffer(device_, tile.events.data(),
                tile.events.size() * sizeof(EventPacket), sizeof(EventPacket));
            tile.flowBuffer = MakeInputBuffer(device_, tile.flowPoints.data(),
                tile.flowPoints.size() * sizeof(std::array<f32, 4>),
                sizeof(std::array<f32, 4>));
            tile.segmentBuffer = MakeInputBuffer(device_, tile.segments.data(),
                tile.segments.size() * sizeof(SegmentPacket), sizeof(SegmentPacket));
            tile.outputBuffer = MakeOutputBuffer(device_,
                static_cast<u64>(outputElements) * sizeof(GpuOutput),
                rhi::MemoryUsage::GpuOnly, rhi::ResourceState::Common);
            tile.readbackBuffer = MakeOutputBuffer(device_,
                static_cast<u64>(outputElements) * sizeof(GpuOutput),
                rhi::MemoryUsage::HostReadback, rhi::ResourceState::Common);
            transferBytes += tile.eventBuffer->SizeBytes();
            transferBytes += tile.flowBuffer->SizeBytes();
            transferBytes += tile.segmentBuffer->SizeBytes();
            transferBytes += 2U * tile.outputBuffer->SizeBytes();
            batchPayloadBytes += tile.eventBuffer->SizeBytes();
            batchPayloadBytes += tile.flowBuffer->SizeBytes();
            batchPayloadBytes += tile.segmentBuffer->SizeBytes();
            batchPayloadBytes += tile.outputBuffer->SizeBytes();
            batchPayloadBytes += tile.readbackBuffer->SizeBytes();
            batch.push_back(std::move(tile));
            batchEnd = tileIndex + 1U;
            if (batchPayloadBytes >= kMaximumSubmissionPayloadBytes)
                break;
        }

        allocator->Reset();
        commandList->Reset(*allocator);
        if (timestamps != nullptr)
        {
            commandList->ResetTimestampQueryPool(*timestamps, 0U, 2U);
            commandList->WriteTimestamp(*timestamps, 0U);
        }
        for (TileWork& tile : batch)
        {
            commandList->Transition(*tile.outputBuffer,
                rhi::ResourceState::Common, rhi::ResourceState::UnorderedAccess);
            commandList->Transition(*tile.readbackBuffer,
                rhi::ResourceState::Common, rhi::ResourceState::CopyDestination);
            commandList->SetComputePipeline(*pipeline_);
            commandList->SetComputeBuffer(0U, *tile.eventBuffer);
            commandList->SetComputeBuffer(1U, *tile.flowBuffer);
            commandList->SetComputeBuffer(2U, *tile.segmentBuffer);
            commandList->SetComputeBuffer(3U, *tile.outputBuffer);
            commandList->SetComputeConstants(tile.constants);
            commandList->Dispatch(
                (tile.width + kThreadGroupSize - 1U) / kThreadGroupSize,
                (tile.height + kThreadGroupSize - 1U) / kThreadGroupSize,
                1U);
            ++dispatches;
            commandList->Transition(*tile.outputBuffer,
                rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopySource);
            commandList->CopyBuffer(*tile.outputBuffer, 0U, *tile.readbackBuffer,
                0U, tile.outputBuffer->SizeBytes());
            commandList->Transition(*tile.readbackBuffer,
                rhi::ResourceState::CopyDestination, rhi::ResourceState::Common);
        }
        if (timestamps != nullptr)
            commandList->WriteTimestamp(*timestamps, 1U);
        commandList->Close();
        queue->Submit(*commandList);
        queue->Signal(*fence, ++fenceValue);
        const auto fenceWaitStarted = std::chrono::steady_clock::now();
        fence->Wait(fenceValue);
        fenceWaitMilliseconds += std::chrono::duration<f64, std::milli>(
            std::chrono::steady_clock::now() - fenceWaitStarted).count();
        if (timestamps != nullptr)
        {
            std::array<u64, 2U> ticks{};
            if (timestamps->TryGetResults(0U, 2U, ticks.data()) && ticks[1] >= ticks[0])
            {
                gpuQueueMilliseconds += static_cast<f64>(ticks[1] - ticks[0]) *
                    device_.TimestampPeriodNanoseconds() * 1.0e-6;
            }
            else
                gpuTimestampAvailable = false;
        }

        for (const TileWork& tile : batch)
        {
            const auto* output = reinterpret_cast<const GpuOutput*>(
                tile.readbackBuffer->Map());
            for (u32 localY = 0U; localY < tile.height; ++localY)
            {
                const i32 y = tile.y + static_cast<i32>(localY);
                const std::size_t row =
                    (static_cast<std::size_t>(tile.face) * stride +
                     static_cast<std::size_t>(y + 1)) * stride;
                for (u32 localX = 0U; localX < tile.width; ++localX)
                {
                    const i32 x = tile.x + static_cast<i32>(localX);
                    const std::size_t source =
                        static_cast<std::size_t>(localY) * tile.width + localX;
                    const std::size_t destination = row + static_cast<std::size_t>(x + 1);
                    StoreGpuOutput(output[source], destination,
                        impactRelief, iceRelief, process);
                }
            }
            tile.readbackBuffer->Unmap();
            ++completedTiles;
            samplesProcessed += static_cast<u64>(tile.width) * tile.height;
        }
        if (reportProgress) reportProgress(completedTiles, tileCount);
        batchStart = batchEnd;
    }

    const f64 seconds = std::chrono::duration<f64>(
        std::chrono::steady_clock::now() - started).count();
    GpuGeologyCompileProduct result;
    result.rasters = std::make_shared<const terrain::BakedGeologyRasters>(
        terrain::BakedGeologyRasters::Build(resolution, recipeHash,
            std::move(impactRelief), std::move(iceRelief), std::move(process)));
    result.samples = samplesProcessed;
    result.eventRecords = eventRecords;
    result.tilesUpdated = completedTiles;
    result.transferBytes = transferBytes;
    result.dispatches = dispatches;
    result.fenceWaitMilliseconds = fenceWaitMilliseconds;
    result.gpuQueueMilliseconds = gpuQueueMilliseconds;
    result.gpuTimestampAvailable = gpuTimestampAvailable;
    result.samplesPerSecond = seconds > 0.0
        ? static_cast<f64>(result.samples) / seconds : 0.0;
    result.eventRecordsPerSecond = seconds > 0.0
        ? static_cast<f64>(result.eventRecords) / seconds : 0.0;
    return result;
}
} // namespace orbit::terrain_gpu
