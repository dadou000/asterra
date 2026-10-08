#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain/BakedRivers.hpp>
#include <orbit/terrain/BakedTectonics.hpp>
#include <orbit/terrain_bake/RiverBaker.hpp>
#include <orbit/terrain_bake/TectonicBaker.hpp>
#include <orbit/world/Planet.hpp>

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
    std::string error;
    f64 lastBakeSeconds{0.0};
    std::filesystem::path path;
};

struct CompletedBake
{
    world::PlanetId planet{};
    std::shared_ptr<const terrain::BakedTectonicRasters> tectonics;
    std::shared_ptr<const terrain::BakedRiverNetwork> rivers;
    // False when the bake was loaded from disk for an older recipe.
    bool matchesRecipe{true};
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

    // Bakes that became active since the last call.
    [[nodiscard]] std::vector<CompletedBake> TakeCompleted();

    [[nodiscard]] std::filesystem::path BakePath(world::PlanetId planet) const;

private:
    struct Entry
    {
        world::PlanetDefinition definition{};
        terrain::AnalyticTerrainDesc desc{};
        BakeSettings settings{};
        u64 currentHash{0};
        std::shared_ptr<const terrain::BakedTectonicRasters> active;
        std::shared_ptr<const terrain::BakedRiverNetwork> activeRivers;
        // River recipe hash for the current terrain recipe on top of `active`.
        u64 currentRiverHash{0};
        f64 staleSeconds{0.0};

        std::thread worker;
        std::unique_ptr<BakeControl> control;
        bool baking{false};
        u64 bakingHash{0};
        // Worker -> main thread handoff, guarded by the service mutex.
        bool workerDone{false};
        std::shared_ptr<const terrain::BakedTectonicRasters> result;
        std::shared_ptr<const terrain::BakedRiverNetwork> resultRivers;
        std::string workerError;
        f64 workerSeconds{0.0};

        u64 failedHash{0};
        std::string error;
        f64 lastBakeSeconds{0.0};
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
    mutable std::mutex mutex_;
    std::map<Key, std::unique_ptr<Entry>> entries_;
    std::map<Key, world::PlanetId> ids_;
    std::vector<CompletedBake> completed_;
};
} // namespace orbit::terrain_bake
