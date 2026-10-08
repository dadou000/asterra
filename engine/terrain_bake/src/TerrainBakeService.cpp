#include <orbit/terrain_bake/TerrainBakeService.hpp>

#include <orbit/terrain_bake/PlanetBakeFile.hpp>

#include <algorithm>
#include <chrono>
#include <format>

namespace orbit::terrain_bake
{
namespace
{
// A recipe that keeps changing (slider drags) must not start a bake per
// frame: wait until it has been stable this long.
constexpr f64 kStableSecondsBeforeAutoBake = 0.75;
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
    return !TectonicsCurrent(entry) || entry.activeRivers == nullptr ||
           entry.activeRivers->RecipeHash() != entry.currentRiverHash;
}

namespace
{
[[nodiscard]] RiverBakeOptions RiverOptionsFor(const BakeSettings& settings) noexcept
{
    RiverBakeOptions options;
    options.resolution = std::clamp(settings.resolution, 16U, 1024U);
    return options;
}
} // namespace

namespace
{
struct BakeProducts
{
    std::shared_ptr<const terrain::BakedTectonicRasters> tectonics;
    std::shared_ptr<const terrain::BakedRiverNetwork> rivers;
};

// Tectonics first (reused when the running one still matches), then the river
// graph on top of them, then one file for both. Null products mean cancelled.
[[nodiscard]] BakeProducts RunBake(
    const world::PlanetDefinition& definition,
    terrain::AnalyticTerrainDesc desc,
    const u32 resolution,
    const std::shared_ptr<const terrain::BakedTectonicRasters>& existing,
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
    BakeSettings settings;
    settings.resolution = resolution;
    products.rivers = BakeRivers(definition, desc, RiverOptionsFor(settings), control);
    if (products.rivers == nullptr)
    {
        return {};
    }
    SavePlanetBake(path, {.tectonics = products.tectonics, .rivers = products.rivers});
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

    entry.definition = definition;
    entry.desc = desc;
    entry.desc.global.bakedTectonics.reset();
    entry.settings = settings;
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
            RefreshRiverHash(entry);
            completed_.push_back({
                .planet = planet,
                .tectonics = entry.active,
                .rivers = entry.activeRivers,
                .matchesRecipe = entry.active->RecipeHash() == hash &&
                    (entry.activeRivers == nullptr ||
                     entry.activeRivers->RecipeHash() == entry.currentRiverHash)});
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
    entry.workerDone = false;
    entry.result.reset();
    entry.resultRivers.reset();
    entry.workerError.clear();
    entry.error.clear();

    const world::PlanetDefinition definition = entry.definition;
    const terrain::AnalyticTerrainDesc desc = entry.desc;
    const std::filesystem::path path = BakePath(planet);
    const auto existing = entry.active;
    BakeControl* const control = entry.control.get();
    Entry* const target = &entry;

    entry.worker = std::thread(
        [this, target, control, definition, desc, path, resolution, existing]()
        {
            const auto started = std::chrono::steady_clock::now();
            BakeProducts products;
            std::string error;
            try
            {
                products = RunBake(definition, desc, resolution, existing, path, control);
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
                RefreshRiverHash(entry);
                completed_.push_back({
                    .planet = planet,
                    .tectonics = entry.active,
                    .rivers = entry.activeRivers,
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
        }

        if (entry.baking)
        {
            // The recipe moved on mid-bake: abandon this one, the normal path
            // below restarts it for the new recipe.
            if (entry.bakingHash != entry.currentHash && entry.control != nullptr)
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
    const u32 resolution = entry->settings.resolution;
    const u64 hash = entry->currentHash;
    const auto existing = entry->active;
    const std::filesystem::path path = BakePath(planet);
    lock.unlock();

    const auto started = std::chrono::steady_clock::now();
    BakeProducts products;
    std::string error;
    try
    {
        products = RunBake(definition, desc, resolution, existing, path, nullptr);
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
    RefreshRiverHash(*entry);
    entry->failedHash = 0;
    entry->error.clear();
    completed_.push_back({
        .planet = planet,
        .tectonics = baked,
        .rivers = products.rivers,
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
    }

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

std::vector<CompletedBake> TerrainBakeService::TakeCompleted()
{
    std::scoped_lock lock(mutex_);
    std::vector<CompletedBake> result = std::move(completed_);
    completed_.clear();
    return result;
}
} // namespace orbit::terrain_bake
