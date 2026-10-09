#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain/BakedRivers.hpp>
#include <orbit/terrain/BakedGeology.hpp>
#include <orbit/terrain/BakedTectonics.hpp>
#include <orbit/terrain_bake/RiverBaker.hpp>
#include <orbit/terrain_bake/TectonicBaker.hpp>
#include <orbit/world/Planet.hpp>

#include <atomic>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace orbit::terrain_bake
{
enum class BakeState : u8
{
    // No bake exists for this planet yet.
    None,
    // A bake is being computed in the background.
    Baking,
    // The active bake matches the current recipe.
    Ready,
    // A bake is active but the recipe has changed since; the old bake keeps
    // driving terrain until a new one finishes and validates.
    Stale,
    // The last bake for the current recipe failed; the active bake (if any)
    // is untouched.
    Failed
};

[[nodiscard]] const char* BakeStateName(BakeState state) noexcept;

struct BakeSettings
{
    u32 resolution{256};
    // Start a bake automatically once the recipe has stopped changing.
    bool autoRebake{true};
    // Stream-power law folded into the river section; part of its recipe.
    RiverIncisionBakeOptions incision{};
};

struct BakeStatus
{
    BakeState state{BakeState::None};
    f32 progress{0.0F};
    BakeSettings settings{};
    u64 currentRecipeHash{0};
    // Recipe the active bake was built from (0 when there is none).
    u64 activeRecipeHash{0};
    u32 activeResolution{0};
    std::size_t activeBytes{0};
    // The river section of the active bake.
    bool riversActive{false};
    u64 activeRiverHash{0};
    u64 currentRiverHash{0};
    u32 riverNodes{0};
    u32 riverSegments{0};
    std::size_t riverBytes{0};
    bool incisionActive{false};
    u32 incisionResolution{0};
    bool geologyActive{false};
    u32 geologyResolution{0};
    u32 geologyLevels{0};
    std::size_t geologyBytes{0};
    bool geologyProcessChannelsActive{false};
    // Work from the most recently activated geological raster compile.
    u64 geologyBakeSamples{0};
    u64 geologyEventRecords{0};
    u64 geologyTilesUpdated{0};
    f64 geologySamplesPerSecond{0.0};
    f64 geologyEventRecordsPerSecond{0.0};
    u64 geologyTransferBytes{0U};
    u64 geologyDispatches{0U};
    f64 geologyFenceWaitMilliseconds{0.0};
    f64 geologyGpuQueueMilliseconds{0.0};
    bool geologyGpuTimestampAvailable{false};
    std::string error;
    f64 lastBakeSeconds{0.0};
    std::filesystem::path path;
};

struct CompletedBake
{
    world::PlanetId planet{};
    std::shared_ptr<const terrain::BakedTectonicRasters> tectonics;
    std::shared_ptr<const terrain::BakedRiverNetwork> rivers;
    std::shared_ptr<const terrain::BakedGeologyRasters> geology;
    // False when the bake was loaded from disk for an older recipe.
    bool matchesRecipe{true};
};

struct GeologyBakeProduct
{
    std::shared_ptr<const terrain::BakedGeologyRasters> rasters;
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

// Synchronous compiler seam for platform GPU batches. Callback references
// remain valid only for the duration of Compile; implementations must finish
// or cancel all queue work before returning and must not retain them.
class GeologyBakeBackend
{
public:
    virtual ~GeologyBakeBackend() = default;
    GeologyBakeBackend(const GeologyBakeBackend&) = delete;
    GeologyBakeBackend& operator=(const GeologyBakeBackend&) = delete;

    [[nodiscard]] virtual GeologyBakeProduct Compile(
        const world::PlanetDefinition& planet,
        const terrain::AnalyticTerrainDesc& desc,
        const terrain::AnalyticTerrainDesc& previousDesc,
        std::shared_ptr<const terrain::BakedGeologyRasters> previous,
        u32 resolution,
        u64 recipeHash,
        const std::function<bool()>& isCancelled,
        const std::function<void(u64, u64)>& reportProgress) const = 0;

protected:
    GeologyBakeBackend() = default;
};

// Owns the baked planet structure for every observed planet: loads it from the
// bake directory, rebakes it in the background when the recipe changes, saves
// it, and hands finished bakes to the main thread. It never touches terrain
// state itself: the owner installs what TakeCompleted returns and rebuilds the
// terrain source, so a failed or cancelled bake always leaves the running
// bake alive.
class TerrainBakeService
{
public:
    explicit TerrainBakeService(std::filesystem::path bakeDirectory);
    ~TerrainBakeService();

    TerrainBakeService(const TerrainBakeService&) = delete;
    TerrainBakeService& operator=(const TerrainBakeService&) = delete;

    // Installs the shared GPU compiler used by subsequent full geology
    // compiles. In-flight workers retain the backend generation they started
    // with; replacing the backend never invalidates an active compiler call.
    void SetGeologyBakeBackend(
        std::shared_ptr<const GeologyBakeBackend> backend);

    // Declares the recipe a planet's bake should match. Cheap; call every
    // time the terrain source is (re)composed. The first call for a planet
    // loads its bake file if one exists.
    void Observe(
        world::PlanetId planet,
        const world::PlanetDefinition& definition,
        const terrain::AnalyticTerrainDesc& desc,
        const BakeSettings& settings);

    // Starts and finishes background work; call from the main thread.
    void Tick(f64 deltaSeconds);

    // Explicit bake. Cancels one in flight. Returns false for an unknown
    // planet or invalid resolution.
    bool StartBake(world::PlanetId planet, std::optional<u32> resolution = {});
    void Cancel(world::PlanetId planet);

    // True once Observe has been called for the planet.
    [[nodiscard]] bool Knows(world::PlanetId planet) const;

    // Bakes on the calling thread and installs the result. Used when terrain
    // must not be generated before a bake exists (first open of a planet).
    // Returns false for an unknown planet or a failed bake.
    [[nodiscard]] bool BakeNow(world::PlanetId planet);

    [[nodiscard]] BakeStatus Status(world::PlanetId planet) const;
    [[nodiscard]] std::shared_ptr<const terrain::BakedTectonicRasters> Active(
        world::PlanetId planet) const;
    [[nodiscard]] std::shared_ptr<const terrain::BakedRiverNetwork> ActiveRivers(
        world::PlanetId planet) const;
    [[nodiscard]] std::shared_ptr<const terrain::BakedGeologyRasters> ActiveGeology(
        world::PlanetId planet) const;

    // Bakes that became active since the last call.
    [[nodiscard]] std::vector<CompletedBake> TakeCompleted();

    [[nodiscard]] std::filesystem::path BakePath(world::PlanetId planet) const;

private:
    struct Entry
    {
        world::PlanetDefinition definition{};
        terrain::AnalyticTerrainDesc desc{};
        terrain::AnalyticTerrainDesc geologyBaseDesc{};
        bool geologyBaseValid{false};
        BakeSettings settings{};
        u64 currentHash{0};
        u64 currentGeologyHash{0};
        std::shared_ptr<const terrain::BakedTectonicRasters> active;
        std::shared_ptr<const terrain::BakedRiverNetwork> activeRivers;
        std::shared_ptr<const terrain::BakedGeologyRasters> activeGeology;
        // River recipe hash for the current terrain recipe on top of `active`.
        u64 currentRiverHash{0};
        f64 staleSeconds{0.0};

        std::thread worker;
        std::unique_ptr<BakeControl> control;
        bool baking{false};
        u64 bakingHash{0};
        u64 bakingGeologyHash{0};
        // Worker -> main thread handoff, guarded by the service mutex.
        bool workerDone{false};
        std::shared_ptr<const terrain::BakedTectonicRasters> result;
        std::shared_ptr<const terrain::BakedRiverNetwork> resultRivers;
        std::shared_ptr<const terrain::BakedGeologyRasters> resultGeology;
        u64 resultGeologyBakeSamples{0};
        u64 resultGeologyEventRecords{0};
        u64 resultGeologyTilesUpdated{0};
        f64 resultGeologySamplesPerSecond{0.0};
        f64 resultGeologyEventRecordsPerSecond{0.0};
        u64 resultGeologyTransferBytes{0U};
        u64 resultGeologyDispatches{0U};
        f64 resultGeologyFenceWaitMilliseconds{0.0};
        f64 resultGeologyGpuQueueMilliseconds{0.0};
        bool resultGeologyGpuTimestampAvailable{false};
        std::string workerError;
        f64 workerSeconds{0.0};

        u64 failedHash{0};
        std::string error;
        f64 lastBakeSeconds{0.0};
        u64 geologyBakeSamples{0};
        u64 geologyEventRecords{0};
        u64 geologyTilesUpdated{0};
        f64 geologySamplesPerSecond{0.0};
        f64 geologyEventRecordsPerSecond{0.0};
        u64 geologyTransferBytes{0U};
        u64 geologyDispatches{0U};
        f64 geologyFenceWaitMilliseconds{0.0};
        f64 geologyGpuQueueMilliseconds{0.0};
        bool geologyGpuTimestampAvailable{false};
    };

    using Key = std::pair<u64, u64>;
    [[nodiscard]] static Key KeyOf(world::PlanetId planet) noexcept;

    Entry* Find(world::PlanetId planet);
    const Entry* Find(world::PlanetId planet) const;
    void Launch(Entry& entry, world::PlanetId planet, u32 resolution);
    void StopWorker(Entry& entry);
    [[nodiscard]] static bool NeedsBake(const Entry& entry) noexcept;
    [[nodiscard]] static bool TectonicsCurrent(const Entry& entry) noexcept;
    static void RefreshRiverHash(Entry& entry);

    std::filesystem::path directory_;
    std::shared_ptr<const GeologyBakeBackend> geologyBakeBackend_;
    mutable std::mutex mutex_;
    std::map<Key, std::unique_ptr<Entry>> entries_;
    std::map<Key, world::PlanetId> ids_;
    std::vector<CompletedBake> completed_;
};
} // namespace orbit::terrain_bake
