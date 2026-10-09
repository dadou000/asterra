#include <orbit/terrain_bake/TerrainBakeService.hpp>

#include <orbit/terrain_bake/PlanetBakeFile.hpp>
#include <orbit/terrain_impacts/ImpactField.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <exception>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <numbers>
#include <thread>
#include <utility>
#include <stdexcept>

namespace orbit::terrain_bake
{
namespace
{
// A recipe that keeps changing (slider drags) must not start a bake per
// frame: wait until it has been stable this long.
constexpr f64 kStableSecondsBeforeAutoBake = 0.75;
constexpr u64 kGeologyBakeAlgorithmVersion = 7U;
constexpr u32 kMaximumParallelGeologyWorkers = 8U;
constexpr u64 kMinimumParallelGeologySamples = 32'768U;
constexpr u32 kGeologyBakeTileSize = 32U;

template<typename SampleTexel>
[[nodiscard]] bool ParallelForGeologyTexels(
    const u32 resolution,
    BakeControl* const control,
    SampleTexel&& sampleTexel)
{
    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    const u32 tilesAcross =
        (static_cast<u32>(stride) + kGeologyBakeTileSize - 1U) /
        kGeologyBakeTileSize;
    const u32 tileCount = 6U * tilesAcross * tilesAcross;
    if (control != nullptr)
    {
        control->rowsDone.store(0U, std::memory_order_relaxed);
        control->rowsTotal.store(
            6U * static_cast<u32>(stride) * tilesAcross,
            std::memory_order_relaxed);
    }

    const u32 reportedWorkers = std::thread::hardware_concurrency();
    const u64 sampleCount = static_cast<u64>(6U) * stride * stride;
    const u32 availableWorkers = std::clamp(
        reportedWorkers == 0U ? 2U : reportedWorkers,
        1U, kMaximumParallelGeologyWorkers);
    const u32 workerCount = sampleCount < kMinimumParallelGeologySamples
        ? 1U
        : std::min(tileCount, availableWorkers);
    std::atomic<u32> nextTile{0U};
    std::atomic<bool> stop{false};
    std::exception_ptr failure;
    std::mutex failureMutex;
    std::vector<std::jthread> workers;
    workers.reserve(workerCount);

    const auto runWorker = [&](const u32 workerIndex)
    {
        terrain_impacts::ImpactQueryScratch scratch;
        try
        {
            while (!stop.load(std::memory_order_relaxed))
            {
                if (control != nullptr &&
                    control->cancel.load(std::memory_order_relaxed))
                {
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
                const u32 tile = nextTile.fetch_add(1U, std::memory_order_relaxed);
                if (tile >= tileCount) break;
                const u32 face = tile / (tilesAcross * tilesAcross);
                const u32 tileInFace = tile % (tilesAcross * tilesAcross);
                const u32 tileX = tileInFace % tilesAcross;
                const u32 tileY = tileInFace / tilesAcross;
                const u32 tileXOffset = tileX * kGeologyBakeTileSize;
                const u32 tileYOffset = tileY * kGeologyBakeTileSize;
                const u32 tileWidth = std::min(
                    kGeologyBakeTileSize, static_cast<u32>(stride) - tileXOffset);
                const u32 tileHeight = std::min(
                    kGeologyBakeTileSize, static_cast<u32>(stride) - tileYOffset);
                sampleTexel(
                    face,
                    static_cast<i32>(tileXOffset) - 1,
                    static_cast<i32>(tileYOffset) - 1,
                    tileWidth,
                    tileHeight,
                    scratch,
                    workerIndex);
                if (control != nullptr)
                    control->rowsDone.fetch_add(tileHeight, std::memory_order_relaxed);
            }
        }
        catch (...)
        {
            {
                std::scoped_lock lock(failureMutex);
                if (failure == nullptr) failure = std::current_exception();
            }
            stop.store(true, std::memory_order_relaxed);
        }
    };

    for (u32 worker = 0U; worker < workerCount; ++worker)
        workers.emplace_back(runWorker, worker);
    for (std::jthread& worker : workers) worker.join();
    if (failure != nullptr) std::rethrow_exception(failure);
    return control == nullptr || !control->cancel.load(std::memory_order_relaxed);
}
} // namespace

const char* BakeStateName(const BakeState state) noexcept
{
    switch (state)
    {
    case BakeState::None: return "none";
    case BakeState::Baking: return "baking";
    case BakeState::Ready: return "ready";
    case BakeState::Stale: return "stale";
    case BakeState::Failed: return "failed";
    }
    return "none";
}

TerrainBakeService::TerrainBakeService(std::filesystem::path bakeDirectory)
    : directory_(std::move(bakeDirectory))
{
}

void TerrainBakeService::SetGeologyBakeBackend(
    std::shared_ptr<const GeologyBakeBackend> backend)
{
    std::scoped_lock lock(mutex_);
    geologyBakeBackend_ = std::move(backend);
}

TerrainBakeService::~TerrainBakeService()
{
    for (auto& [key, entry] : entries_)
    {
        static_cast<void>(key);
        StopWorker(*entry);
    }
}

TerrainBakeService::Key TerrainBakeService::KeyOf(
    const world::PlanetId planet) noexcept
{
    return {planet.high, planet.low};
}

TerrainBakeService::Entry* TerrainBakeService::Find(
    const world::PlanetId planet)
{
    const auto found = entries_.find(KeyOf(planet));
    return found == entries_.end() ? nullptr : found->second.get();
}

const TerrainBakeService::Entry* TerrainBakeService::Find(
    const world::PlanetId planet) const
{
    const auto found = entries_.find(KeyOf(planet));
    return found == entries_.end() ? nullptr : found->second.get();
}

std::filesystem::path TerrainBakeService::BakePath(
    const world::PlanetId planet) const
{
    return directory_ /
           std::format("{:016x}{:016x}.orbitbake", planet.high, planet.low);
}

bool TerrainBakeService::TectonicsCurrent(const Entry& entry) noexcept
{
    return entry.active != nullptr &&
           entry.active->RecipeHash() == entry.currentHash &&
           entry.active->Resolution() == entry.settings.resolution;
}

bool TerrainBakeService::NeedsBake(const Entry& entry) noexcept
{
    const bool hasGeology = entry.desc.impactHistory != nullptr || entry.desc.iceFractures != nullptr;
    const u64 expectedHash = terrain::StableCombine64(
        terrain::TerrainRecipeHash(entry.definition, entry.desc),
        terrain::StableCombine64(entry.settings.resolution, kGeologyBakeAlgorithmVersion));
    const bool geologyCurrent = hasGeology
        ? entry.activeGeology != nullptr && entry.activeGeology->RecipeHash() == expectedHash &&
              entry.activeGeology->Resolution() == entry.settings.resolution &&
              entry.activeGeology->HasProcessChannels()
        : entry.activeGeology == nullptr;
    return !TectonicsCurrent(entry) || entry.activeRivers == nullptr ||
           entry.activeRivers->RecipeHash() != entry.currentRiverHash || !geologyCurrent;
}

namespace
{
[[nodiscard]] RiverBakeOptions RiverOptionsFor(const BakeSettings& settings) noexcept
{
    RiverBakeOptions options;
    options.resolution = std::clamp(settings.resolution, 16U, 1024U);
    options.incision = settings.incision;
    return options;
}
} // namespace

namespace
{
struct BakeProducts
{
    std::shared_ptr<const terrain::BakedTectonicRasters> tectonics;
    std::shared_ptr<const terrain::BakedRiverNetwork> rivers;
    std::shared_ptr<const terrain::BakedGeologyRasters> geology;
    u64 geologySamples{0U};
    u64 geologyEventRecords{0U};
    u64 geologyTilesUpdated{0U};
    f64 geologySamplesPerSecond{0.0};
    f64 geologyEventRecordsPerSecond{0.0};
    u64 geologyTransferBytes{0U};
    u64 geologyDispatches{0U};
    f64 geologyFenceWaitMilliseconds{0.0};
    f64 geologyGpuQueueMilliseconds{0.0};
    bool geologyGpuTimestampAvailable{false};
};

struct GeologyBakeMetrics
{
    u64 samples{0U};
    u64 eventRecords{0U};
    u64 tilesUpdated{0U};
    f64 samplesPerSecond{0.0};
    f64 eventRecordsPerSecond{0.0};
    u64 transferBytes{0U};
    u64 dispatches{0U};
    f64 fenceWaitMilliseconds{0.0};
    f64 gpuQueueMilliseconds{0.0};
    bool gpuTimestampAvailable{false};
};

struct ImpactInfluenceCap
{
    math::Double3 center{};
    f64 angularRadiusRadians{0.0};
};

[[nodiscard]] std::string ImpactHistoryWithoutAuthoredImpacts(
    terrain_impacts::ImpactFieldDefinition definition)
{
    definition.authoredImpacts.clear();
    definition.resurfacingEvents.clear();
    return terrain_impacts::SerializeImpactFieldToml(definition);
}

[[nodiscard]] std::string ImpactRecordSignature(
    const terrain_impacts::ImpactFieldDefinition& base,
    const terrain_impacts::ImpactRecord& impact)
{
    auto single = base;
    single.resurfacingEvents.clear();
    single.authoredImpacts.assign(1U, impact);
    return terrain_impacts::SerializeImpactFieldToml(single);
}

[[nodiscard]] std::string ResurfacingRecordSignature(
    const terrain_impacts::ImpactFieldDefinition& base,
    const terrain_impacts::ResurfacingRecord& event)
{
    auto single = base;
    single.authoredImpacts.clear();
    single.resurfacingEvents.assign(1U, event);
    return terrain_impacts::SerializeImpactFieldToml(single);
}

[[nodiscard]] f64 EffectiveImpactRadius(
    const terrain_impacts::ImpactRecord& impact,
    const terrain_impacts::ImpactFieldDefinition& definition)
{
    if (impact.impactorDiameterMeters <= 0.0) return impact.radiusMeters;
    return terrain_impacts::ScaleImpactCraterRadiusMeters({
        .impactorDiameterMeters = impact.impactorDiameterMeters,
        .impactVelocityMetersPerSecond = impact.impactVelocityMetersPerSecond,
        .impactAngleDegrees = impact.impactAngleDegrees,
        .impactorDensityKgPerCubicMeter = impact.impactorDensityKgPerCubicMeter,
        .targetDensityKgPerCubicMeter = definition.targetDensityKgPerCubicMeter,
        .surfaceGravityMetersPerSecondSquared = definition.surfaceGravityMetersPerSecondSquared,
        .targetStrengthPascals = definition.targetStrengthPascals});
}

[[nodiscard]] f64 ImpactInfluenceRadius(
    const terrain_impacts::ImpactRecord& impact,
    const terrain_impacts::ImpactFieldDefinition& definition)
{
    const f64 radius = EffectiveImpactRadius(impact, definition);
    const f64 extent = impact.InfluenceExtentRadii();
    const f64 profileBound = 1.65 /
        std::max(1.0 - impact.shapeIrregularity, 0.75) * 1.65;
    const f64 main = profileBound * extent;
    const f64 binary = impact.binarySeparationRadii +
        profileBound * extent * std::max(impact.binaryCompanionRadiusRatio, 0.0);
    const f64 secondaries = impact.secondaryCount > 0U
        ? 5.5 + 1.8 * profileBound *
            std::max(impact.secondaryRadiusRatio, 0.0) * extent
        : 0.0;
    return radius * std::max({main, binary, secondaries});
}

[[nodiscard]] std::optional<std::vector<ImpactInfluenceCap>> ChangedImpactCaps(
    const world::PlanetDefinition& planet,
    const std::shared_ptr<const terrain_impacts::ImpactFieldDefinition>& before,
    const std::shared_ptr<const terrain_impacts::ImpactFieldDefinition>& after)
{
    if (before == nullptr || after == nullptr ||
        ImpactHistoryWithoutAuthoredImpacts(*before) !=
            ImpactHistoryWithoutAuthoredImpacts(*after))
        return std::nullopt;

    using ImpactKey = std::pair<u64, u64>;
    std::map<ImpactKey, const terrain_impacts::ImpactRecord*> oldEvents;
    std::map<ImpactKey, const terrain_impacts::ImpactRecord*> newEvents;
    for (const auto& impact : before->authoredImpacts)
        oldEvents[{impact.id.high, impact.id.low}] = &impact;
    for (const auto& impact : after->authoredImpacts)
        newEvents[{impact.id.high, impact.id.low}] = &impact;

    std::vector<ImpactInfluenceCap> caps;
    const auto include = [&](const terrain_impacts::ImpactRecord& impact,
                             const terrain_impacts::ImpactFieldDefinition& definition)
    {
        const f64 extent = std::min(
            ImpactInfluenceRadius(impact, definition) / planet.radiusMeters,
            std::numbers::pi_v<f64>);
        caps.push_back({math::Normalize(impact.centerUnitDirection), extent});
    };
    for (const auto& [key, oldImpact] : oldEvents)
    {
        const auto found = newEvents.find(key);
        if (found != newEvents.end() &&
            ImpactRecordSignature(*before, *oldImpact) ==
                ImpactRecordSignature(*after, *found->second))
            continue;
        include(*oldImpact, *before);
        if (found != newEvents.end()) include(*found->second, *after);
    }
    for (const auto& [key, newImpact] : newEvents)
    {
        if (!oldEvents.contains(key)) include(*newImpact, *after);
    }

    const auto flowCaps = [&](const terrain_impacts::ResurfacingRecord& event)
    {
        std::vector<ImpactInfluenceCap> result;
        if (event.centerlineUnitDirections.empty()) return result;
        const f64 widthAngle = std::min(
            event.widthMeters * 2.0 / planet.radiusMeters,
            std::numbers::pi_v<f64>);
        const auto append = [&](const math::Double3& center, const f64 segmentRadius)
        {
            result.push_back({math::Normalize(center), std::min(
                widthAngle + segmentRadius, std::numbers::pi_v<f64>)});
        };
        for (const auto& direction : event.centerlineUnitDirections)
            append(direction, 0.0);
        for (std::size_t i = 1U; i < event.centerlineUnitDirections.size(); ++i)
        {
            const auto& a = event.centerlineUnitDirections[i - 1U];
            const auto& b = event.centerlineUnitDirections[i];
            const f64 halfSegmentAngle = 0.5 * std::acos(std::clamp(
                math::Dot(math::Normalize(a), math::Normalize(b)), -1.0, 1.0));
            const math::Double3 midpoint = a + b;
            if (math::LengthSquared(midpoint) > 1.0e-12)
                append(midpoint, halfSegmentAngle);
            else
                append(a, halfSegmentAngle);
        }
        return result;
    };
    std::map<ImpactKey, const terrain_impacts::ResurfacingRecord*> oldFlows;
    std::map<ImpactKey, const terrain_impacts::ResurfacingRecord*> newFlows;
    for (const auto& event : before->resurfacingEvents)
        oldFlows[{event.id.high, event.id.low}] = &event;
    for (const auto& event : after->resurfacingEvents)
        newFlows[{event.id.high, event.id.low}] = &event;
    for (const auto& [key, oldEvent] : oldFlows)
    {
        const auto found = newFlows.find(key);
        if (found != newFlows.end() &&
            ResurfacingRecordSignature(*before, *oldEvent) ==
                ResurfacingRecordSignature(*after, *found->second))
            continue;
        const auto oldCaps = flowCaps(*oldEvent);
        caps.insert(caps.end(), oldCaps.begin(), oldCaps.end());
        if (found != newFlows.end())
        {
            const auto newCaps = flowCaps(*found->second);
            caps.insert(caps.end(), newCaps.begin(), newCaps.end());
        }
    }
    for (const auto& [key, newEvent] : newFlows)
    {
        if (oldFlows.contains(key)) continue;
        const auto newCaps = flowCaps(*newEvent);
        caps.insert(caps.end(), newCaps.begin(), newCaps.end());
    }
    return caps;
}

[[nodiscard]] u64 GeologyRecipeHash(
    const world::PlanetDefinition& definition,
    const terrain::AnalyticTerrainDesc& desc,
    const u32 resolution)
{
    const u64 hash = terrain::StableCombine64(
        terrain::TerrainRecipeHash(definition, desc),
        terrain::StableCombine64(resolution, kGeologyBakeAlgorithmVersion));
    return hash == 0U ? 1U : hash;
}

[[nodiscard]] std::shared_ptr<const terrain::BakedGeologyRasters> BakeGeology(
    const world::PlanetDefinition& definition,
    const terrain::AnalyticTerrainDesc& desc,
    const u32 resolution,
    BakeControl* const control,
    GeologyBakeMetrics* const metrics)
{
    const terrain_impacts::IceFractureDefinition* iceDefinition = desc.iceFractures.get();
    if (iceDefinition == nullptr && desc.impactHistory != nullptr && desc.impactHistory->iceFractures != nullptr)
        iceDefinition = desc.impactHistory->iceFractures.get();
    const bool hasIce = iceDefinition != nullptr && iceDefinition->enabled;
    if (desc.impactHistory == nullptr && !hasIce) return nullptr;

    std::unique_ptr<terrain_impacts::ImpactField> impact;
    if (desc.impactHistory != nullptr)
    {
        impact = std::make_unique<terrain_impacts::ImpactField>(definition, *desc.impactHistory);
        if (metrics != nullptr)
        {
            metrics->eventRecords += static_cast<u64>(impact->ResolvedImpacts().size());
            metrics->eventRecords += static_cast<u64>(desc.impactHistory->resurfacingEvents.size());
        }
    }
    std::unique_ptr<terrain_impacts::IceFractureField> ice;
    if (hasIce)
    {
        ice = std::make_unique<terrain_impacts::IceFractureField>(definition, *iceDefinition);
        if (metrics != nullptr)
            metrics->eventRecords += static_cast<u64>(ice->SegmentCount());
    }

    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    std::vector<f32> impactRelief(6U * stride * stride, 0.0F);
    std::vector<f32> iceRelief(6U * stride * stride, 0.0F);
    std::vector<terrain::BakedGeologyProcessTexel> processRaster(
        6U * stride * stride);
    const f64 footprint = 2.0 * definition.radiusMeters / static_cast<f64>(resolution);
    const auto samplingStarted = std::chrono::steady_clock::now();
    const bool completed = ParallelForGeologyTexels(
        resolution, control,
        [&](const u32 face, const i32 tileX, const i32 tileY,
            const u32 tileWidth, const u32 tileHeight,
            terrain_impacts::ImpactQueryScratch& scratch,
            const u32)
    {
        const i32 centerX = tileX + static_cast<i32>(tileWidth / 2U);
        const i32 centerY = tileY + static_cast<i32>(tileHeight / 2U);
        const math::Double3 centerDirection =
            terrain::BakedGeologyTexelDirection(face, centerX, centerY, resolution);
        f64 tileAngularRadius = 0.0;
        const std::array<std::pair<i32, i32>, 4U> corners{{
            {tileX, tileY},
            {tileX + static_cast<i32>(tileWidth) - 1, tileY},
            {tileX, tileY + static_cast<i32>(tileHeight) - 1},
            {tileX + static_cast<i32>(tileWidth) - 1,
             tileY + static_cast<i32>(tileHeight) - 1}}};
        for (const auto& [cornerX, cornerY] : corners)
        {
            const math::Double3 corner = terrain::BakedGeologyTexelDirection(
                face, cornerX, cornerY, resolution);
            tileAngularRadius = std::max(tileAngularRadius,
                std::acos(std::clamp(math::Dot(centerDirection, corner), -1.0, 1.0)));
        }
        tileAngularRadius = std::min(tileAngularRadius +
            2.0 * footprint / definition.radiusMeters,
            std::numbers::pi_v<f64>);
        if (impact != nullptr)
            impact->CollectEventsIntersectingCap(
                centerDirection, tileAngularRadius, scratch, scratch.eventBatch);
        else
            scratch.eventBatch.clear();
        if (ice != nullptr)
            ice->CollectSegmentsIntersectingCap(
                centerDirection, tileAngularRadius, scratch, scratch.fractureBatch);
        else
            scratch.fractureBatch.clear();

        for (u32 localY = 0U; localY < tileHeight; ++localY)
        {
            const i32 y = tileY + static_cast<i32>(localY);
            const std::size_t rowIndex =
                (static_cast<std::size_t>(face) * stride +
                 static_cast<std::size_t>(y + 1)) * stride;
            for (u32 localX = 0U; localX < tileWidth; ++localX)
            {
                const i32 x = tileX + static_cast<i32>(localX);
                const std::size_t index = rowIndex + static_cast<std::size_t>(x + 1);
                const math::Double3 direction =
                    terrain::BakedGeologyTexelDirection(face, x, y, resolution);
                terrain_impacts::CraterProcessSample impactSample = impact != nullptr
                    ? impact->Sample(direction, footprint, scratch, scratch.eventBatch)
                    : terrain_impacts::CraterProcessSample{};
                const terrain_impacts::IceFractureSample iceSample = ice != nullptr
                    ? ice->Sample(direction, footprint, scratch.fractureBatch)
                    : terrain_impacts::IceFractureSample{};
                const bool fracturesAfterSurface = iceSample.damage > 0.0 &&
                    iceSample.ageOrder >= impactSample.exposureAgeOrder;
                const f64 fractureRetention = fracturesAfterSurface || iceSample.damage <= 0.0
                    ? 1.0
                    : std::clamp(1.0 - impactSample.resurfacedMaterialFraction, 0.0, 1.0) *
                        std::clamp(1.0 - impactSample.excavationCoverage, 0.0, 1.0);
                if (fracturesAfterSurface)
                {
                    impactSample.exposureAgeOrder = iceSample.ageOrder;
                    impactSample.exposureAgeYears = std::max(
                        0.0, (desc.impactHistory != nullptr
                            ? desc.impactHistory->surfaceAgeYears
                            : iceSample.formationAgeYears) - iceSample.formationAgeYears);
                }
                impactRelief[index] = static_cast<f32>(impactSample.heightDeltaMeters);
                iceRelief[index] = static_cast<f32>(iceSample.heightDeltaMeters * fractureRetention);
                processRaster[index] = {
                    .excavationDepthMeters = static_cast<f32>(impactSample.excavationDepthMeters),
                    .ejectaThicknessMeters = static_cast<f32>(impactSample.ejectaThicknessMeters),
                    .debrisField = static_cast<f32>(impactSample.debrisField),
                    .rayField = static_cast<f32>(impactSample.rayField),
                    .meltThicknessMeters = static_cast<f32>(impactSample.meltThicknessMeters),
                    .brecciaField = static_cast<f32>(impactSample.brecciaField),
                    .resurfacedMaterialFraction = static_cast<f32>(impactSample.resurfacedMaterialFraction),
                    .resurfacingThicknessMeters = static_cast<f32>(impactSample.resurfacingThicknessMeters),
                    .microImpactRoughnessMeters = static_cast<f32>(impactSample.microImpactRoughnessMeters),
                    .microImpactCoverage = static_cast<f32>(impactSample.microImpactCoverage),
                    .excavationCoverage = static_cast<f32>(impactSample.excavationCoverage),
                    .formationAgeYears = static_cast<f32>(impactSample.formationAgeYears),
                    .exposureAgeYears = static_cast<f32>(impactSample.exposureAgeYears),
                    .formationAgeOrder = impactSample.formationAgeOrder,
                    .exposureAgeOrder = impactSample.exposureAgeOrder,
                    .affectingImpacts = impactSample.affectingImpacts,
                    .iceDamage = static_cast<f32>(iceSample.damage * fractureRetention),
                    .fractureCoverage = static_cast<f32>(iceSample.fractureCoverage * fractureRetention),
                    .nearbySegments = iceSample.nearbySegments};
            }
        }
    });
    if (!completed)
        return nullptr;
    if (metrics != nullptr)
    {
        metrics->samples += static_cast<u64>(6U) *
            static_cast<u64>(stride) * static_cast<u64>(stride);
        const u32 tilesAcross =
            (static_cast<u32>(stride) + kGeologyBakeTileSize - 1U) /
            kGeologyBakeTileSize;
        metrics->tilesUpdated = static_cast<u64>(6U) * tilesAcross * tilesAcross;
    }
    if (metrics != nullptr)
    {
        const f64 seconds = std::chrono::duration<f64>(
            std::chrono::steady_clock::now() - samplingStarted).count();
        metrics->samplesPerSecond = seconds > 0.0
            ? static_cast<f64>(metrics->samples) / seconds
            : 0.0;
        metrics->eventRecordsPerSecond = seconds > 0.0
            ? static_cast<f64>(metrics->eventRecords) / seconds
            : 0.0;
    }
    return std::make_shared<const terrain::BakedGeologyRasters>(
        terrain::BakedGeologyRasters::Build(
            resolution, GeologyRecipeHash(definition, desc, resolution),
            std::move(impactRelief), std::move(iceRelief), std::move(processRaster)));
}

[[nodiscard]] terrain::BakedGeologyProcessTexel EncodeProcessTexel(
    const terrain_impacts::CraterProcessSample& impact,
    const terrain_impacts::IceFractureSample& ice) noexcept
{
    return {
        .excavationDepthMeters = static_cast<f32>(impact.excavationDepthMeters),
        .ejectaThicknessMeters = static_cast<f32>(impact.ejectaThicknessMeters),
        .debrisField = static_cast<f32>(impact.debrisField),
        .rayField = static_cast<f32>(impact.rayField),
        .meltThicknessMeters = static_cast<f32>(impact.meltThicknessMeters),
        .brecciaField = static_cast<f32>(impact.brecciaField),
        .resurfacedMaterialFraction = static_cast<f32>(impact.resurfacedMaterialFraction),
        .resurfacingThicknessMeters = static_cast<f32>(impact.resurfacingThicknessMeters),
        .microImpactRoughnessMeters = static_cast<f32>(impact.microImpactRoughnessMeters),
        .microImpactCoverage = static_cast<f32>(impact.microImpactCoverage),
        .excavationCoverage = static_cast<f32>(impact.excavationCoverage),
        .formationAgeYears = static_cast<f32>(impact.formationAgeYears),
        .exposureAgeYears = static_cast<f32>(impact.exposureAgeYears),
        .formationAgeOrder = impact.formationAgeOrder,
        .exposureAgeOrder = impact.exposureAgeOrder,
        .affectingImpacts = impact.affectingImpacts,
        .iceDamage = static_cast<f32>(ice.damage),
        .fractureCoverage = static_cast<f32>(ice.fractureCoverage),
        .nearbySegments = ice.nearbySegments};
}

[[nodiscard]] std::shared_ptr<const terrain::BakedGeologyRasters> TryBakeLocalImpactEdits(
    const world::PlanetDefinition& planet,
    const terrain::AnalyticTerrainDesc& beforeDesc,
    const terrain::AnalyticTerrainDesc& afterDesc,
    const u32 resolution,
    const std::shared_ptr<const terrain::BakedGeologyRasters>& previous,
    BakeControl* const control,
    GeologyBakeMetrics* const metrics)
{
    if (previous == nullptr || !previous->HasProcessChannels() ||
        previous->Resolution() != resolution ||
        beforeDesc.impactHistory == nullptr || afterDesc.impactHistory == nullptr ||
        beforeDesc.iceFractures != afterDesc.iceFractures ||
        previous->RecipeHash() != GeologyRecipeHash(planet, beforeDesc, resolution))
        return nullptr;

    const auto caps = ChangedImpactCaps(
        planet, beforeDesc.impactHistory, afterDesc.impactHistory);
    if (!caps.has_value()) return nullptr;

    terrain_impacts::ImpactField impact(planet, *afterDesc.impactHistory);
    if (metrics != nullptr)
    {
        metrics->eventRecords = static_cast<u64>(impact.ResolvedImpacts().size()) +
            static_cast<u64>(afterDesc.impactHistory->resurfacingEvents.size());
    }
    std::vector<f32> impactRelief = previous->ReliefGutter();
    std::vector<f32> iceRelief = previous->IceReliefGutter();
    std::vector<terrain::BakedGeologyProcessTexel> processRaster = previous->ProcessGutter();
    const std::size_t stride = static_cast<std::size_t>(resolution) + 2U;
    const f64 footprint = 2.0 * planet.radiusMeters / static_cast<f64>(resolution);
    const f64 haloAngle = std::min(footprint / planet.radiusMeters,
        std::numbers::pi_v<f64>);
    std::vector<f64> capCosines;
    capCosines.reserve(caps->size());
    for (const ImpactInfluenceCap& cap : *caps)
        capCosines.push_back(std::cos(std::min(
            cap.angularRadiusRadians + haloAngle,
            std::numbers::pi_v<f64>)));

    const auto started = std::chrono::steady_clock::now();
    std::array<u64, kMaximumParallelGeologyWorkers> resampledTexels{};
    std::array<u64, kMaximumParallelGeologyWorkers> updatedTiles{};
    const bool completed = ParallelForGeologyTexels(
        resolution, control,
        [&](const u32 face, const i32 tileX, const i32 tileY,
            const u32 tileWidth, const u32 tileHeight,
            terrain_impacts::ImpactQueryScratch& scratch,
            const u32 workerIndex)
    {
        const i32 centerX = tileX + static_cast<i32>(tileWidth / 2U);
        const i32 centerY = tileY + static_cast<i32>(tileHeight / 2U);
        const math::Double3 centerDirection =
            terrain::BakedGeologyTexelDirection(face, centerX, centerY, resolution);
        f64 tileAngularRadius = 0.0;
        const std::array<std::pair<i32, i32>, 4U> corners{{
            {tileX, tileY},
            {tileX + static_cast<i32>(tileWidth) - 1, tileY},
            {tileX, tileY + static_cast<i32>(tileHeight) - 1},
            {tileX + static_cast<i32>(tileWidth) - 1,
             tileY + static_cast<i32>(tileHeight) - 1}}};
        for (const auto& [cornerX, cornerY] : corners)
        {
            const math::Double3 corner = terrain::BakedGeologyTexelDirection(
                face, cornerX, cornerY, resolution);
            tileAngularRadius = std::max(tileAngularRadius,
                std::acos(std::clamp(math::Dot(centerDirection, corner), -1.0, 1.0)));
        }
        tileAngularRadius = std::min(tileAngularRadius + haloAngle,
            std::numbers::pi_v<f64>);
        impact.CollectEventsIntersectingCap(
            centerDirection, tileAngularRadius, scratch, scratch.eventBatch);
        bool tileChanged = false;
        for (u32 localY = 0U; localY < tileHeight; ++localY)
        {
            const i32 y = tileY + static_cast<i32>(localY);
            const std::size_t rowIndex =
                (static_cast<std::size_t>(face) * stride +
                 static_cast<std::size_t>(y + 1)) * stride;
            for (u32 localX = 0U; localX < tileWidth; ++localX)
            {
                const i32 x = tileX + static_cast<i32>(localX);
                const math::Double3 direction =
                    terrain::BakedGeologyTexelDirection(face, x, y, resolution);
                bool affected = false;
                for (std::size_t capIndex = 0U; capIndex < caps->size(); ++capIndex)
                {
                    if (math::Dot(direction, (*caps)[capIndex].center) >= capCosines[capIndex])
                    {
                        affected = true;
                        break;
                    }
                }
                if (!affected) continue;
                const std::size_t index = rowIndex + static_cast<std::size_t>(x + 1);
                const auto sample = impact.Sample(
                    direction, footprint, scratch, scratch.eventBatch);
                impactRelief[index] = static_cast<f32>(sample.heightDeltaMeters);
                const auto previousProcess = processRaster[index];
                auto updatedProcess = EncodeProcessTexel(sample, {});
                updatedProcess.iceDamage = previousProcess.iceDamage;
                updatedProcess.fractureCoverage = previousProcess.fractureCoverage;
                updatedProcess.nearbySegments = previousProcess.nearbySegments;
                processRaster[index] = updatedProcess;
                ++resampledTexels[workerIndex];
                tileChanged = true;
            }
        }
        if (tileChanged) ++updatedTiles[workerIndex];
    });
    if (!completed) return nullptr;
    if (metrics != nullptr)
    {
        for (u64 workerSamples : resampledTexels)
            metrics->samples += workerSamples;
        for (u64 workerTiles : updatedTiles)
            metrics->tilesUpdated += workerTiles;
    }
    if (metrics != nullptr)
    {
        const f64 seconds = std::chrono::duration<f64>(
            std::chrono::steady_clock::now() - started).count();
        metrics->samplesPerSecond = seconds > 0.0
            ? static_cast<f64>(metrics->samples) / seconds
            : 0.0;
        metrics->eventRecordsPerSecond = seconds > 0.0
            ? static_cast<f64>(metrics->eventRecords) / seconds
            : 0.0;
    }
    return std::make_shared<const terrain::BakedGeologyRasters>(
        terrain::BakedGeologyRasters::Build(
            resolution, GeologyRecipeHash(planet, afterDesc, resolution),
            std::move(impactRelief), std::move(iceRelief), std::move(processRaster)));
}

// Tectonics first (reused when the running one still matches), then the river
// graph on top of them, then one file for both. Null products mean cancelled.
[[nodiscard]] BakeProducts RunBake(
    const world::PlanetDefinition& definition,
    terrain::AnalyticTerrainDesc desc,
    const terrain::AnalyticTerrainDesc& previousGeologyDesc,
    const u32 resolution,
    const BakeSettings& incisionSettings,
    const std::shared_ptr<const terrain::BakedTectonicRasters>& existing,
    const std::shared_ptr<const terrain::BakedGeologyRasters>& existingGeology,
    const std::shared_ptr<const GeologyBakeBackend>& geologyBackend,
    const std::filesystem::path& path,
    BakeControl* const control)
{
    BakeProducts products;
    if (existing != nullptr &&
        existing->RecipeHash() == terrain::TectonicBakeRecipeHash(definition, desc) &&
        existing->Resolution() == resolution)
    {
        products.tectonics = existing;
    }
    else
    {
        products.tectonics = BakeTectonics(definition, desc, {.resolution = resolution}, control);
    }
    if (products.tectonics == nullptr)
    {
        return {};
    }

    desc.global.bakedTectonics = products.tectonics;
    BakeSettings settings = incisionSettings;
    settings.resolution = resolution;
    products.rivers = BakeRivers(definition, desc, RiverOptionsFor(settings), control);
    if (products.rivers == nullptr)
    {
        return {};
    }
    terrain::AnalyticTerrainDesc geologyDesc = desc;
    geologyDesc.global.bakedTectonics.reset();
    geologyDesc.bakedRivers.reset();
    geologyDesc.bakedGeology.reset();
    const u64 geologyHash = GeologyRecipeHash(definition, geologyDesc, resolution);
    if ((desc.impactHistory != nullptr || desc.iceFractures != nullptr) && existingGeology != nullptr &&
        existingGeology->RecipeHash() == geologyHash && existingGeology->Resolution() == resolution)
        products.geology = existingGeology;
    else
    {
        GeologyBakeMetrics metrics;
        const terrain_impacts::IceFractureDefinition* iceDefinition =
            geologyDesc.iceFractures.get();
        if (iceDefinition == nullptr && geologyDesc.impactHistory != nullptr &&
            geologyDesc.impactHistory->iceFractures != nullptr)
            iceDefinition = geologyDesc.impactHistory->iceFractures.get();
        const bool hasGeologyInput = geologyDesc.impactHistory != nullptr ||
            (iceDefinition != nullptr && iceDefinition->enabled);
        if (products.geology == nullptr && hasGeologyInput && geologyBackend != nullptr)
        {
            const auto isCancelled = [control]
            {
                return control != nullptr &&
                    control->cancel.load(std::memory_order_relaxed);
            };
            const auto reportProgress = [control](const u64 done, const u64 total)
            {
                if (control == nullptr) return;
                control->rowsDone.store(static_cast<u32>(std::min<u64>(
                    done, std::numeric_limits<u32>::max())),
                    std::memory_order_relaxed);
                control->rowsTotal.store(static_cast<u32>(std::min<u64>(
                    total, std::numeric_limits<u32>::max())),
                    std::memory_order_relaxed);
            };
            GeologyBakeProduct compiled = geologyBackend->Compile(
                definition, geologyDesc, previousGeologyDesc, existingGeology,
                resolution, geologyHash,
                isCancelled, reportProgress);
            if (compiled.rasters == nullptr)
            {
                if (isCancelled()) return {};
                throw std::runtime_error("The geology bake backend returned no raster.");
            }
            if (compiled.rasters->Resolution() != resolution ||
                compiled.rasters->RecipeHash() != geologyHash ||
                !compiled.rasters->HasProcessChannels())
            {
                throw std::runtime_error(
                    "The geology bake backend returned a raster that does not match "
                    "the requested recipe, resolution, or process-channel contract.");
            }
            products.geology = std::move(compiled.rasters);
            metrics.samples = compiled.samples;
            metrics.eventRecords = compiled.eventRecords;
            metrics.tilesUpdated = compiled.tilesUpdated;
            metrics.samplesPerSecond = compiled.samplesPerSecond;
            metrics.eventRecordsPerSecond = compiled.eventRecordsPerSecond;
            metrics.transferBytes = compiled.transferBytes;
            metrics.dispatches = compiled.dispatches;
            metrics.fenceWaitMilliseconds = compiled.fenceWaitMilliseconds;
            metrics.gpuQueueMilliseconds = compiled.gpuQueueMilliseconds;
            metrics.gpuTimestampAvailable = compiled.gpuTimestampAvailable;
        }
        else if (products.geology == nullptr)
        {
            products.geology = TryBakeLocalImpactEdits(
                definition, previousGeologyDesc, geologyDesc, resolution,
                existingGeology, control, &metrics);
            if (products.geology == nullptr && control != nullptr &&
                control->cancel.load(std::memory_order_relaxed))
                return {};
            if (products.geology == nullptr)
                products.geology = BakeGeology(
                    definition, geologyDesc, resolution, control, &metrics);
        }
        products.geologySamples = metrics.samples;
        products.geologyEventRecords = metrics.eventRecords;
        products.geologyTilesUpdated = metrics.tilesUpdated;
        products.geologySamplesPerSecond = metrics.samplesPerSecond;
        products.geologyEventRecordsPerSecond = metrics.eventRecordsPerSecond;
        products.geologyTransferBytes = metrics.transferBytes;
        products.geologyDispatches = metrics.dispatches;
        products.geologyFenceWaitMilliseconds = metrics.fenceWaitMilliseconds;
        products.geologyGpuQueueMilliseconds = metrics.gpuQueueMilliseconds;
        products.geologyGpuTimestampAvailable = metrics.gpuTimestampAvailable;
    }
    if (control != nullptr && control->cancel.load(std::memory_order_relaxed)) return {};
    SavePlanetBake(path, {.tectonics = products.tectonics, .rivers = products.rivers, .geology = products.geology});
    return products;
}
} // namespace

void TerrainBakeService::RefreshRiverHash(Entry& entry)
{
    terrain::AnalyticTerrainDesc desc = entry.desc;
    desc.global.bakedTectonics = entry.active;
    desc.bakedRivers.reset();
    entry.currentRiverHash = RiverBakeRecipeHash(
        entry.definition, desc, RiverOptionsFor(entry.settings));
}

void TerrainBakeService::StopWorker(Entry& entry)
{
    if (entry.control != nullptr)
    {
        entry.control->cancel.store(true, std::memory_order_relaxed);
    }
    if (entry.worker.joinable())
    {
        entry.worker.join();
    }
}

void TerrainBakeService::Observe(
    const world::PlanetId planet,
    const world::PlanetDefinition& definition,
    const terrain::AnalyticTerrainDesc& desc,
    const BakeSettings& settings)
{
    std::unique_lock lock(mutex_);

    auto& slot = entries_[KeyOf(planet)];
    const bool first = slot == nullptr;
    if (first)
    {
        slot = std::make_unique<Entry>();
        ids_[KeyOf(planet)] = planet;
    }
    Entry& entry = *slot;

    terrain::AnalyticTerrainDesc observedDesc = desc;
    observedDesc.global.bakedTectonics.reset();
    observedDesc.bakedRivers.reset();
    observedDesc.bakedGeology.reset();
    const u64 observedGeologyHash = GeologyRecipeHash(
        definition, observedDesc, settings.resolution);
    if (!first && entry.currentGeologyHash != 0U &&
        observedGeologyHash != entry.currentGeologyHash && !entry.geologyBaseValid)
    {
        entry.geologyBaseDesc = entry.desc;
        entry.geologyBaseValid = true;
    }

    entry.definition = definition;
    entry.desc = std::move(observedDesc);
    entry.settings = settings;
    entry.currentGeologyHash = observedGeologyHash;
    const u64 hash = terrain::TectonicBakeRecipeHash(definition, entry.desc);
    const bool recipeChanged = hash != entry.currentHash;
    entry.currentHash = hash;
    if (recipeChanged)
    {
        entry.staleSeconds = 0.0;
    }
    RefreshRiverHash(entry);

    if (first)
    {
        // Pick up a bake saved by an earlier session. One for an older recipe
        // is still installed: it keeps the terrain stable until the rebake
        // finishes.
        std::string loadError;
        if (auto loaded = LoadPlanetBake(BakePath(planet), &loadError);
            loaded.has_value() && loaded->tectonics != nullptr)
        {
            entry.active = loaded->tectonics;
            entry.activeRivers = loaded->rivers;
            entry.activeGeology = loaded->geology;
            RefreshRiverHash(entry);
            completed_.push_back({
                .planet = planet,
                .tectonics = entry.active,
                .rivers = entry.activeRivers,
                .geology = entry.activeGeology,
                .matchesRecipe = !NeedsBake(entry)});
        }
    }
}

void TerrainBakeService::Launch(
    Entry& entry,
    const world::PlanetId planet,
    const u32 resolution)
{
    StopWorker(entry);

    entry.control = std::make_unique<BakeControl>();
    entry.baking = true;
    entry.bakingHash = entry.currentHash;
    entry.bakingGeologyHash = entry.currentGeologyHash;
    entry.workerDone = false;
    entry.result.reset();
    entry.resultRivers.reset();
    entry.resultGeology.reset();
    entry.workerError.clear();
    entry.error.clear();

    const world::PlanetDefinition definition = entry.definition;
    const terrain::AnalyticTerrainDesc desc = entry.desc;
    const terrain::AnalyticTerrainDesc previousGeologyDesc =
        entry.geologyBaseValid ? entry.geologyBaseDesc : entry.desc;
    const std::filesystem::path path = BakePath(planet);
    const auto existing = entry.active;
    const auto existingGeology = entry.activeGeology;
    const auto geologyBackend = geologyBakeBackend_;
    const BakeSettings settings = entry.settings;
    BakeControl* const control = entry.control.get();
    Entry* const target = &entry;

    entry.worker = std::thread(
        [this, target, control, definition, desc, previousGeologyDesc, path, resolution, settings, existing, existingGeology, geologyBackend]()
        {
            const auto started = std::chrono::steady_clock::now();
            BakeProducts products;
            std::string error;
            try
            {
                products = RunBake(definition, desc, previousGeologyDesc, resolution, settings,
                    existing, existingGeology, geologyBackend, path, control);
            }
            catch (const std::exception& exception)
            {
                error = exception.what();
                products = {};
            }

            const f64 seconds = std::chrono::duration<f64>(
                std::chrono::steady_clock::now() - started).count();
            std::scoped_lock lock(mutex_);
            target->result = std::move(products.tectonics);
            target->resultRivers = std::move(products.rivers);
            target->resultGeology = std::move(products.geology);
            target->resultGeologyBakeSamples = products.geologySamples;
            target->resultGeologyEventRecords = products.geologyEventRecords;
            target->resultGeologyTilesUpdated = products.geologyTilesUpdated;
            target->resultGeologySamplesPerSecond = products.geologySamplesPerSecond;
            target->resultGeologyEventRecordsPerSecond = products.geologyEventRecordsPerSecond;
            target->resultGeologyTransferBytes = products.geologyTransferBytes;
            target->resultGeologyDispatches = products.geologyDispatches;
            target->resultGeologyFenceWaitMilliseconds = products.geologyFenceWaitMilliseconds;
            target->resultGeologyGpuQueueMilliseconds = products.geologyGpuQueueMilliseconds;
            target->resultGeologyGpuTimestampAvailable = products.geologyGpuTimestampAvailable;
            target->workerError = std::move(error);
            target->workerSeconds = seconds;
            target->workerDone = true;
        });
}

void TerrainBakeService::Tick(const f64 deltaSeconds)
{
    std::unique_lock lock(mutex_);
    for (auto& [key, slot] : entries_)
    {
        Entry& entry = *slot;
        const world::PlanetId planet = ids_.at(key);

        if (entry.baking && entry.workerDone)
        {
            lock.unlock();
            if (entry.worker.joinable())
            {
                entry.worker.join();
            }
            lock.lock();

            entry.baking = false;
            entry.lastBakeSeconds = entry.workerSeconds;
            if (entry.result != nullptr)
            {
                // Validated by construction (finite layers, matching sizes);
                // only now does it replace the running bake.
                entry.active = entry.result;
                entry.activeRivers = entry.resultRivers;
                entry.activeGeology = entry.resultGeology;
                entry.geologyBakeSamples = entry.resultGeologyBakeSamples;
                entry.geologyEventRecords = entry.resultGeologyEventRecords;
                entry.geologyTilesUpdated = entry.resultGeologyTilesUpdated;
                entry.geologySamplesPerSecond = entry.resultGeologySamplesPerSecond;
                entry.geologyEventRecordsPerSecond = entry.resultGeologyEventRecordsPerSecond;
                entry.geologyTransferBytes = entry.resultGeologyTransferBytes;
                entry.geologyDispatches = entry.resultGeologyDispatches;
                entry.geologyFenceWaitMilliseconds = entry.resultGeologyFenceWaitMilliseconds;
                entry.geologyGpuQueueMilliseconds = entry.resultGeologyGpuQueueMilliseconds;
                entry.geologyGpuTimestampAvailable = entry.resultGeologyGpuTimestampAvailable;
                entry.geologyBaseValid = false;
                RefreshRiverHash(entry);
                completed_.push_back({
                    .planet = planet,
                    .tectonics = entry.active,
                    .rivers = entry.activeRivers,
                    .geology = entry.activeGeology,
                    .matchesRecipe = !NeedsBake(entry)});
                entry.failedHash = 0;
            }
            else if (!entry.workerError.empty())
            {
                entry.error = entry.workerError;
                entry.failedHash = entry.bakingHash;
            }
            entry.result.reset();
            entry.resultRivers.reset();
            entry.resultGeology.reset();
        }

        if (entry.baking)
        {
            // The recipe moved on mid-bake: abandon this one, the normal path
            // below restarts it for the new recipe.
            if ((entry.bakingHash != entry.currentHash ||
                    entry.bakingGeologyHash != entry.currentGeologyHash) &&
                entry.control != nullptr)
            {
                entry.control->cancel.store(true, std::memory_order_relaxed);
            }
            continue;
        }

        if (entry.settings.autoRebake &&
            NeedsBake(entry) &&
            entry.failedHash != entry.currentHash)
        {
            entry.staleSeconds += deltaSeconds;
            // A bake that is missing altogether starts at once; edits to an
            // existing bake wait for the recipe to settle.
            if (entry.active == nullptr ||
                entry.staleSeconds >= kStableSecondsBeforeAutoBake)
            {
                Launch(entry, planet, entry.settings.resolution);
                entry.staleSeconds = 0.0;
            }
        }
        else
        {
            entry.staleSeconds = 0.0;
        }
    }
}

bool TerrainBakeService::StartBake(
    const world::PlanetId planet,
    const std::optional<u32> resolution)
{
    std::unique_lock lock(mutex_);
    Entry* entry = Find(planet);
    if (entry == nullptr)
    {
        return false;
    }

    TectonicBakeOptions options;
    options.resolution = resolution.value_or(entry->settings.resolution);
    if (!options.IsValid())
    {
        return false;
    }
    entry->settings.resolution = options.resolution;
    entry->failedHash = 0;
    entry->staleSeconds = 0.0;

    lock.unlock();
    Launch(*entry, planet, options.resolution);
    return true;
}

void TerrainBakeService::Cancel(const world::PlanetId planet)
{
    std::scoped_lock lock(mutex_);
    Entry* entry = Find(planet);
    if (entry != nullptr && entry->control != nullptr && entry->baking)
    {
        entry->control->cancel.store(true, std::memory_order_relaxed);
        // A cancelled bake must not simply restart: remember it as failed for
        // this recipe until the recipe changes or the user bakes again.
        entry->failedHash = entry->currentHash;
        entry->error = "The bake was cancelled.";
    }
}

bool TerrainBakeService::Knows(const world::PlanetId planet) const
{
    std::scoped_lock lock(mutex_);
    return Find(planet) != nullptr;
}

bool TerrainBakeService::BakeNow(const world::PlanetId planet)
{
    std::unique_lock lock(mutex_);
    Entry* entry = Find(planet);
    if (entry == nullptr)
    {
        return false;
    }

    const world::PlanetDefinition definition = entry->definition;
    const terrain::AnalyticTerrainDesc desc = entry->desc;
    const terrain::AnalyticTerrainDesc previousGeologyDesc =
        entry->geologyBaseValid ? entry->geologyBaseDesc : entry->desc;
    const u32 resolution = entry->settings.resolution;
    const u64 hash = entry->currentHash;
    const auto existing = entry->active;
    const auto existingGeology = entry->activeGeology;
    const auto geologyBackend = geologyBakeBackend_;
    const BakeSettings settings = entry->settings;
    const std::filesystem::path path = BakePath(planet);
    lock.unlock();

    const auto started = std::chrono::steady_clock::now();
    BakeProducts products;
    std::string error;
    try
    {
        products = RunBake(definition, desc, previousGeologyDesc, resolution, settings,
            existing, existingGeology, geologyBackend, path, nullptr);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        products = {};
    }
    const auto baked = products.tectonics;
    const f64 seconds = std::chrono::duration<f64>(
        std::chrono::steady_clock::now() - started).count();

    lock.lock();
    entry = Find(planet);
    if (entry == nullptr)
    {
        return false;
    }
    entry->lastBakeSeconds = seconds;
    if (baked == nullptr)
    {
        entry->error = error.empty() ? "The bake did not complete." : error;
        entry->failedHash = hash;
        return false;
    }

    entry->active = baked;
    entry->activeRivers = products.rivers;
    entry->activeGeology = products.geology;
    entry->geologyBakeSamples = products.geologySamples;
    entry->geologyEventRecords = products.geologyEventRecords;
    entry->geologyTilesUpdated = products.geologyTilesUpdated;
    entry->geologySamplesPerSecond = products.geologySamplesPerSecond;
    entry->geologyEventRecordsPerSecond = products.geologyEventRecordsPerSecond;
    entry->geologyTransferBytes = products.geologyTransferBytes;
    entry->geologyDispatches = products.geologyDispatches;
    entry->geologyFenceWaitMilliseconds = products.geologyFenceWaitMilliseconds;
    entry->geologyGpuQueueMilliseconds = products.geologyGpuQueueMilliseconds;
    entry->geologyGpuTimestampAvailable = products.geologyGpuTimestampAvailable;
    entry->geologyBaseValid = false;
    RefreshRiverHash(*entry);
    entry->failedHash = 0;
    entry->error.clear();
    completed_.push_back({
        .planet = planet,
        .tectonics = baked,
        .rivers = products.rivers,
        .geology = products.geology,
        .matchesRecipe = !NeedsBake(*entry)});
    return true;
}

BakeStatus TerrainBakeService::Status(const world::PlanetId planet) const
{
    std::scoped_lock lock(mutex_);
    const Entry* entry = Find(planet);
    BakeStatus status;
    if (entry == nullptr)
    {
        return status;
    }

    status.settings = entry->settings;
    status.currentRecipeHash = entry->currentHash;
    status.path = BakePath(planet);
    status.error = entry->error;
    status.lastBakeSeconds = entry->lastBakeSeconds;
    if (entry->active != nullptr)
    {
        status.activeRecipeHash = entry->active->RecipeHash();
        status.activeResolution = entry->active->Resolution();
        status.activeBytes = entry->active->ByteSize();
    }
    status.currentRiverHash = entry->currentRiverHash;
    if (entry->activeRivers != nullptr)
    {
        status.riversActive = true;
        status.activeRiverHash = entry->activeRivers->RecipeHash();
        status.riverNodes = static_cast<u32>(entry->activeRivers->Nodes().size());
        status.riverSegments = static_cast<u32>(entry->activeRivers->Segments().size());
        status.riverBytes = entry->activeRivers->ByteSize();
        status.incisionActive = entry->activeRivers->HasIncision();
        status.incisionResolution = entry->activeRivers->IncisionResolution();
    }
    if (entry->activeGeology != nullptr)
    {
        status.geologyActive = true;
        status.geologyResolution = entry->activeGeology->Resolution();
        status.geologyLevels = entry->activeGeology->LevelCount();
        status.geologyBytes = entry->activeGeology->ByteSize();
        status.geologyProcessChannelsActive = entry->activeGeology->HasProcessChannels();
    }
    status.geologyBakeSamples = entry->geologyBakeSamples;
    status.geologyEventRecords = entry->geologyEventRecords;
    status.geologyTilesUpdated = entry->geologyTilesUpdated;
    status.geologySamplesPerSecond = entry->geologySamplesPerSecond;
    status.geologyEventRecordsPerSecond = entry->geologyEventRecordsPerSecond;
    status.geologyTransferBytes = entry->geologyTransferBytes;
    status.geologyDispatches = entry->geologyDispatches;
    status.geologyFenceWaitMilliseconds = entry->geologyFenceWaitMilliseconds;
    status.geologyGpuQueueMilliseconds = entry->geologyGpuQueueMilliseconds;
    status.geologyGpuTimestampAvailable = entry->geologyGpuTimestampAvailable;

    if (entry->baking)
    {
        status.state = BakeState::Baking;
        status.progress =
            entry->control != nullptr ? entry->control->Progress() : 0.0F;
    }
    else if (entry->active != nullptr && !NeedsBake(*entry))
    {
        status.state = BakeState::Ready;
        status.progress = 1.0F;
    }
    else if (entry->failedHash == entry->currentHash &&
             entry->failedHash != 0U)
    {
        status.state = BakeState::Failed;
    }
    else if (entry->active != nullptr)
    {
        status.state = BakeState::Stale;
        status.progress = 1.0F;
    }
    return status;
}

std::shared_ptr<const terrain::BakedTectonicRasters> TerrainBakeService::Active(
    const world::PlanetId planet) const
{
    std::scoped_lock lock(mutex_);
    const Entry* entry = Find(planet);
    return entry == nullptr ? nullptr : entry->active;
}

std::shared_ptr<const terrain::BakedRiverNetwork> TerrainBakeService::ActiveRivers(
    const world::PlanetId planet) const
{
    std::scoped_lock lock(mutex_);
    const Entry* entry = Find(planet);
    return entry == nullptr ? nullptr : entry->activeRivers;
}

std::shared_ptr<const terrain::BakedGeologyRasters> TerrainBakeService::ActiveGeology(
    const world::PlanetId planet) const
{
    std::scoped_lock lock(mutex_);
    const Entry* entry = Find(planet);
    return entry == nullptr ? nullptr : entry->activeGeology;
}

std::vector<CompletedBake> TerrainBakeService::TakeCompleted()
{
    std::scoped_lock lock(mutex_);
    std::vector<CompletedBake> result = std::move(completed_);
    completed_.clear();
    return result;
}
} // namespace orbit::terrain_bake
