#include <orbit/studio_session/StudioTerrainRebuildScheduler.hpp>

#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace orbit::studio_session
{
namespace
{
constexpr u32 kMaximumBoundedRadiusTiles = 64U;

[[nodiscard]] constexpr auto AllProductsMask() noexcept
{
    terrain_dependency::TerrainDependencyProductMask mask = 0U;

    for (u32 index = 0U;
         index < static_cast<u32>(
             terrain_dependency::TerrainDependencyProduct::Count);
         ++index)
    {
        mask |=
            terrain_dependency::ProductBit(
                static_cast<
                    terrain_dependency::TerrainDependencyProduct>(
                        index));
    }

    return mask;
}

[[nodiscard]] u32 ProductCount(
    const terrain_dependency::TerrainDependencyProductMask mask) noexcept
{
    return static_cast<u32>(
        std::popcount(mask));
}

[[nodiscard]] u64 RevisionFingerprint(
    const terrain_dependency::TerrainDependencyGraph& graph,
    const terrain::PhysicalTerrainPageAddress& address) noexcept
{
    const auto revisions =
        graph.Revisions(address);

    return revisions.has_value()
        ? terrain::RevisionFingerprint(*revisions)
        : 0U;
}

[[nodiscard]] TerrainRebuildState AggregateState(
    const StudioTerrainBodyRebuildStatus& status) noexcept
{
    if (status.failedPages > 0U)
    {
        return TerrainRebuildState::Failed;
    }

    if (status.stalePages > 0U)
    {
        return TerrainRebuildState::StaleReplaced;
    }

    if (status.uploadingPages > 0U)
    {
        return TerrainRebuildState::Uploading;
    }

    if (status.buildingPages > 0U)
    {
        return TerrainRebuildState::BuildingGpu;
    }

    if (status.queuedPages > 0U)
    {
        return TerrainRebuildState::Queued;
    }

    if (status.dirtyPages > 0U)
    {
        return TerrainRebuildState::Dirty;
    }

    if (status.readyPages > 0U)
    {
        return TerrainRebuildState::Ready;
    }

    return TerrainRebuildState::Clean;
}
} // namespace

const char* TerrainRebuildStateName(
    const TerrainRebuildState state) noexcept
{
    switch (state)
    {
    case TerrainRebuildState::Clean:
        return "Clean";
    case TerrainRebuildState::Dirty:
        return "Dirty";
    case TerrainRebuildState::Queued:
        return "Queued";
    case TerrainRebuildState::BuildingCpu:
        return "Building CPU";
    case TerrainRebuildState::BuildingGpu:
        return "Building GPU";
    case TerrainRebuildState::Uploading:
        return "Uploading";
    case TerrainRebuildState::Ready:
        return "Ready";
    case TerrainRebuildState::Failed:
        return "Failed";
    case TerrainRebuildState::StaleReplaced:
        return "Stale/replaced";
    }

    return "Unknown";
}

bool StudioTerrainRebuildConfig::IsValid() const noexcept
{
    return
        std::isfinite(editDebounceSeconds) &&
        editDebounceSeconds >= 0.0 &&
        maxBuildRequestsPerTick > 0U;
}

StudioTerrainRebuildScheduler::StudioTerrainRebuildScheduler(
    terrain_dependency::TerrainDependencyGraph& graph,
    const StudioTerrainRebuildConfig config)
    : graph_(&graph),
      config_(config)
{
    if (!config_.IsValid())
    {
        throw std::invalid_argument(
            "M06 Studio terrain rebuild scheduler config is invalid.");
    }
}

StudioTerrainRebuildScheduler::PageEntry*
StudioTerrainRebuildScheduler::FindPage(
    const terrain::PhysicalTerrainPageAddress& address) noexcept
{
    const auto found =
        std::find_if(
            pages_.begin(),
            pages_.end(),
            [&address](const PageEntry& page)
            {
                return page.address == address;
            });

    return found != pages_.end()
        ? &*found
        : nullptr;
}

const StudioTerrainRebuildScheduler::PageEntry*
StudioTerrainRebuildScheduler::FindPage(
    const terrain::PhysicalTerrainPageAddress& address) const noexcept
{
    const auto found =
        std::find_if(
            pages_.begin(),
            pages_.end(),
            [&address](const PageEntry& page)
            {
                return page.address == address;
            });

    return found != pages_.end()
        ? &*found
        : nullptr;
}

void StudioTerrainRebuildScheduler::RegisterPage(
    const terrain::PhysicalTerrainPageAddress& address,
    const terrain::TerrainGenerationRevisions revisions)
{
    if (!address.planet.IsValid())
    {
        throw std::invalid_argument(
            "M06 cannot register an invalid terrain page.");
    }

    if (ContainsPage(address))
    {
        return;
    }

    if (!graph_->ContainsPage(address))
    {
        graph_->RegisterPage(
            address,
            revisions);
    }

    PageEntry page{
        .address = address
    };

    for (u32 index = 0U;
         index < static_cast<u32>(
             terrain_dependency::TerrainDependencyProduct::Count);
         ++index)
    {
        const auto product =
            static_cast<
                terrain_dependency::TerrainDependencyProduct>(
                    index);

        const auto status =
            graph_->Status(
                address,
                product);

        if (!status.has_value() ||
            status->state !=
                procedural_graph::NodeState::Clean)
        {
            page.dirtyProducts |=
                terrain_dependency::ProductBit(
                    product);
        }

        if (status.has_value())
        {
            page.observedStaleCompletions[index] =
                status->staleCompletions;
        }
    }

    page.cycleProducts =
        page.dirtyProducts;

    pages_.push_back(
        std::move(page));
}

bool StudioTerrainRebuildScheduler::ContainsPage(
    const terrain::PhysicalTerrainPageAddress& address) const noexcept
{
    return FindPage(address) != nullptr;
}

bool StudioTerrainRebuildScheduler::AddressMatches(
    const terrain::PhysicalTerrainPageAddress& address,
    const terrain_dependency::TerrainSpatialInvalidationScope& scope)
{
    if (address.planet != scope.planet)
    {
        return false;
    }

    if (scope.global)
    {
        return true;
    }

    if (address.tile.level !=
        scope.center.level)
    {
        return false;
    }

    const u32 radius =
        std::min(
            kMaximumBoundedRadiusTiles,
            scope.radiusTiles +
                scope.downstreamRadiusTiles);

    const auto neighborhood =
        world::TileNeighborhood(
            scope.center,
            radius);

    return std::find(
               neighborhood.begin(),
               neighborhood.end(),
               address.tile) !=
        neighborhood.end();
}

void StudioTerrainRebuildScheduler::QueueChange(
    const terrain_dependency::TerrainInvalidationRequest& request)
{
    if (!request.scope.IsValid())
    {
        throw std::invalid_argument(
            "M06 cannot queue an invalid terrain change.");
    }

    const auto products =
        terrain_dependency::
            TerrainDependencyGraph::
                ProductsForChange(
                    request.kind);

    for (auto& page : pages_)
    {
        if (!AddressMatches(
                page.address,
                request.scope))
        {
            continue;
        }

        page.pendingProducts |=
            products;
        page.cycleProducts |=
            products;
    }

    auto found =
        std::find_if(
            pendingChanges_.begin(),
            pendingChanges_.end(),
            [&request](
                const PendingChange& pending)
            {
                if (pending.request.kind !=
                        request.kind ||
                    pending.request.scope.planet !=
                        request.scope.planet ||
                    pending.request.scope.global !=
                        request.scope.global)
                {
                    return false;
                }

                if (request.scope.global)
                {
                    return true;
                }

                return
                    pending.request.scope.center ==
                        request.scope.center &&
                    pending.request.scope.radiusTiles ==
                        request.scope.radiusTiles &&
                    pending.request.scope.downstreamRadiusTiles ==
                        request.scope.downstreamRadiusTiles;
            });

    if (found != pendingChanges_.end())
    {
        found->remainingSeconds =
            config_.editDebounceSeconds;
        return;
    }

    pendingChanges_.push_back({
        .request = request,
        .remainingSeconds =
            config_.editDebounceSeconds
    });
}

void StudioTerrainRebuildScheduler::FlushChanges(
    const bool all)
{
    for (auto iterator =
             pendingChanges_.begin();
         iterator !=
             pendingChanges_.end();)
    {
        if (!all &&
            iterator->remainingSeconds > 0.0)
        {
            ++iterator;
            continue;
        }

        const auto request =
            iterator->request;

        const auto result =
            graph_->ApplyChange(
                request);

        for (auto& page : pages_)
        {
            if (!AddressMatches(
                    page.address,
                    request.scope))
            {
                continue;
            }

            page.pendingProducts &=
                ~result.dirtyProducts;
            page.dirtyProducts |=
                result.dirtyProducts;
            page.requestedProducts &=
                ~result.dirtyProducts;
            page.cycleProducts |=
                result.dirtyProducts;

            page.uploadFailed = false;
            page.uploadError.clear();

            if (page.uploading)
            {
                page.uploading = false;
                ++page.staleRejected;
                page.stalePulseTicks = 2U;
            }
        }

        iterator =
            pendingChanges_.erase(
                iterator);
    }
}

void StudioTerrainRebuildScheduler::RefreshPage(
    PageEntry& page)
{
    for (u32 index = 0U;
         index < static_cast<u32>(
             terrain_dependency::TerrainDependencyProduct::Count);
         ++index)
    {
        const auto product =
            static_cast<
                terrain_dependency::TerrainDependencyProduct>(
                    index);
        const auto bit =
            terrain_dependency::ProductBit(
                product);

        const auto status =
            graph_->Status(
                page.address,
                product);

        if (!status.has_value())
        {
            continue;
        }

        if (status->staleCompletions >
            page.observedStaleCompletions[index])
        {
            page.staleRejected +=
                status->staleCompletions -
                page.observedStaleCompletions[index];
            page.stalePulseTicks = 2U;
        }

        page.observedStaleCompletions[index] =
            status->staleCompletions;

        if ((page.dirtyProducts & bit) != 0U &&
            status->state ==
                procedural_graph::NodeState::Clean)
        {
            page.dirtyProducts &= ~bit;
            page.requestedProducts &= ~bit;
        }

        if (status->state ==
            procedural_graph::NodeState::Failed)
        {
            page.requestedProducts &= ~bit;
        }
    }
}

void StudioTerrainRebuildScheduler::RefreshAll()
{
    for (auto& page : pages_)
    {
        RefreshPage(page);
    }
}

std::optional<
    terrain_dependency::TerrainDependencyProduct>
StudioTerrainRebuildScheduler::NextTarget(
    const PageEntry& page) const noexcept
{
    using Product =
        terrain_dependency::
            TerrainDependencyProduct;

    constexpr std::array<Product, 7U>
        priority{
            Product::SurfaceMaterial,
            Product::Scatter,
            Product::BiomeWeights,
            Product::ExposedSurface,
            Product::TerrainProcesses,
            Product::Drainage,
            Product::Geology
        };

    for (const Product product :
         priority)
    {
        const auto bit =
            terrain_dependency::ProductBit(
                product);

        if ((page.dirtyProducts & bit) != 0U &&
            (page.requestedProducts & bit) == 0U)
        {
            return product;
        }
    }

    return std::nullopt;
}

void StudioTerrainRebuildScheduler::ScheduleBudget()
{
    if (paused_)
    {
        return;
    }

    u32 submitted = 0U;

    for (auto& page : pages_)
    {
        if (submitted >=
            config_.maxBuildRequestsPerTick)
        {
            break;
        }

        if (page.uploading ||
            page.uploadFailed ||
            page.dirtyProducts == 0U)
        {
            continue;
        }

        const auto target =
            NextTarget(page);

        if (!target.has_value())
        {
            continue;
        }

        graph_->RequestBuild(
            page.address,
            *target);

        page.requestedProducts |=
            terrain_dependency::ProductBit(
                *target);

        ++submitted;
    }
}

void StudioTerrainRebuildScheduler::Tick(
    const f64 deltaSeconds)
{
    if (!std::isfinite(deltaSeconds) ||
        deltaSeconds < 0.0)
    {
        throw std::invalid_argument(
            "M06 rebuild scheduler delta time must be finite and non-negative.");
    }

    graph_->Poll();
    RefreshAll();

    for (auto& pending :
         pendingChanges_)
    {
        pending.remainingSeconds =
            std::max(
                0.0,
                pending.remainingSeconds -
                    deltaSeconds);
    }

    FlushChanges(false);
    RefreshAll();

    ScheduleBudget();
    graph_->Poll();
    RefreshAll();

    for (auto& page : pages_)
    {
        if (page.stalePulseTicks > 0U)
        {
            --page.stalePulseTicks;
        }
    }
}

void StudioTerrainRebuildScheduler::RebuildDirty()
{
    graph_->Poll();
    RefreshAll();

    FlushChanges(true);
    RefreshAll();

    ScheduleBudget();
    graph_->Poll();
    RefreshAll();
}

void StudioTerrainRebuildScheduler::SetPaused(
    const bool paused) noexcept
{
    paused_ = paused;
}

bool StudioTerrainRebuildScheduler::Paused() const noexcept
{
    return paused_;
}

std::optional<u64>
StudioTerrainRebuildScheduler::BeginUpload(
    const terrain::PhysicalTerrainPageAddress& address)
{
    PageEntry* page =
        FindPage(address);

    if (page == nullptr ||
        page->uploading ||
        page->uploadFailed ||
        page->pendingProducts != 0U ||
        page->dirtyProducts != 0U)
    {
        return std::nullopt;
    }

    const u64 revision =
        RevisionFingerprint(
            *graph_,
            address);

    if (revision == 0U)
    {
        return std::nullopt;
    }

    page->uploading = true;
    page->uploadRevisionFingerprint =
        revision;

    return revision;
}

bool StudioTerrainRebuildScheduler::CompleteUpload(
    const terrain::PhysicalTerrainPageAddress& address,
    const u64 revisionFingerprint,
    const bool success,
    const std::string_view error)
{
    PageEntry* page =
        FindPage(address);

    if (page == nullptr)
    {
        return false;
    }

    const u64 currentRevision =
        RevisionFingerprint(
            *graph_,
            address);

    const bool current =
        page->uploading &&
        page->uploadRevisionFingerprint ==
            revisionFingerprint &&
        currentRevision ==
            revisionFingerprint;

    if (!current)
    {
        if (page->uploading &&
            page->uploadRevisionFingerprint ==
                revisionFingerprint)
        {
            page->uploading = false;
        }

        ++page->staleRejected;
        page->stalePulseTicks = 2U;
        return false;
    }

    page->uploading = false;

    if (!success)
    {
        page->uploadFailed = true;
        page->uploadError =
            error.empty()
                ? "Terrain upload failed."
                : std::string(error);
        return false;
    }

    page->uploadFailed = false;
    page->uploadError.clear();
    return true;
}

StudioTerrainPageRebuildStatus
StudioTerrainRebuildScheduler::MakeStatus(
    const PageEntry& page) const
{
    StudioTerrainPageRebuildStatus result{
        .address = page.address,
        .dirtyProducts =
            page.dirtyProducts |
            page.pendingProducts,
        .revisionFingerprint =
            RevisionFingerprint(
                *graph_,
                page.address),
        .staleRejected =
            page.staleRejected
    };

    bool buildingCpu = false;
    bool buildingGpu = false;
    bool failed = false;

    for (u32 index = 0U;
         index < static_cast<u32>(
             terrain_dependency::TerrainDependencyProduct::Count);
         ++index)
    {
        const auto product =
            static_cast<
                terrain_dependency::TerrainDependencyProduct>(
                    index);

        const auto status =
            graph_->Status(
                page.address,
                product);

        if (!status.has_value())
        {
            continue;
        }

        if (status->state ==
            procedural_graph::NodeState::Building)
        {
            if (status->backend ==
                procedural_graph::ExecutionBackend::Gpu)
            {
                buildingGpu = true;
            }
            else
            {
                buildingCpu = true;
            }
        }

        if (status->state ==
            procedural_graph::NodeState::Failed)
        {
            failed = true;

            if (result.error.empty())
            {
                result.error =
                    status->error;
            }
        }
    }

    if (page.uploadFailed)
    {
        failed = true;
        result.error =
            page.uploadError;
    }

    if (failed)
    {
        result.state =
            TerrainRebuildState::Failed;
    }
    else if (page.stalePulseTicks > 0U)
    {
        result.state =
            TerrainRebuildState::StaleReplaced;
    }
    else if (page.uploading)
    {
        result.state =
            TerrainRebuildState::Uploading;
    }
    else if (buildingGpu)
    {
        result.state =
            TerrainRebuildState::BuildingGpu;
    }
    else if (buildingCpu)
    {
        result.state =
            TerrainRebuildState::BuildingCpu;
    }
    else if (page.requestedProducts != 0U)
    {
        result.state =
            TerrainRebuildState::Queued;
    }
    else if (result.dirtyProducts != 0U)
    {
        result.state =
            TerrainRebuildState::Dirty;
    }
    else if (page.cycleProducts != 0U)
    {
        result.state =
            TerrainRebuildState::Ready;
    }
    else
    {
        result.state =
            TerrainRebuildState::Clean;
    }

    result.totalProducts =
        ProductCount(
            page.cycleProducts);

    const auto remaining =
        page.cycleProducts &
        result.dirtyProducts;

    result.completedProducts =
        result.totalProducts -
        ProductCount(remaining);

    return result;
}

std::optional<StudioTerrainPageRebuildStatus>
StudioTerrainRebuildScheduler::PageStatus(
    const terrain::PhysicalTerrainPageAddress& address) const
{
    const PageEntry* page =
        FindPage(address);

    if (page == nullptr)
    {
        return std::nullopt;
    }

    return MakeStatus(*page);
}

std::vector<StudioTerrainPageRebuildStatus>
StudioTerrainRebuildScheduler::Catalog() const
{
    std::vector<
        StudioTerrainPageRebuildStatus>
        result;

    result.reserve(
        pages_.size());

    for (const auto& page : pages_)
    {
        result.push_back(
            MakeStatus(page));
    }

    return result;
}

StudioTerrainBodyRebuildStatus
StudioTerrainRebuildScheduler::BodyStatus(
    const world::PlanetId planet) const
{
    StudioTerrainBodyRebuildStatus result{
        .planet = planet,
        .paused = paused_
    };

    bool hasCpuBuild = false;
    bool hasGpuBuild = false;

    for (const auto& page :
         pages_)
    {
        if (page.address.planet !=
            planet)
        {
            continue;
        }

        const auto status =
            MakeStatus(page);

        ++result.pages;
        result.completedProducts +=
            status.completedProducts;
        result.totalProducts +=
            status.totalProducts;
        result.staleRejected +=
            status.staleRejected;

        switch (status.state)
        {
        case TerrainRebuildState::Dirty:
            ++result.dirtyPages;
            break;
        case TerrainRebuildState::Queued:
            ++result.queuedPages;
            break;
        case TerrainRebuildState::BuildingCpu:
            ++result.buildingPages;
            hasCpuBuild = true;
            break;
        case TerrainRebuildState::BuildingGpu:
            ++result.buildingPages;
            hasGpuBuild = true;
            break;
        case TerrainRebuildState::Uploading:
            ++result.uploadingPages;
            break;
        case TerrainRebuildState::Ready:
            ++result.readyPages;
            break;
        case TerrainRebuildState::Failed:
            ++result.failedPages;
            break;
        case TerrainRebuildState::StaleReplaced:
            ++result.stalePages;
            break;
        case TerrainRebuildState::Clean:
            break;
        }
    }

    result.progress =
        result.totalProducts > 0U
            ? static_cast<f64>(
                  result.completedProducts) /
                static_cast<f64>(
                    result.totalProducts)
            : 1.0;

    result.state =
        AggregateState(result);

    if (result.buildingPages > 0U)
    {
        result.state =
            hasGpuBuild
                ? TerrainRebuildState::BuildingGpu
                : TerrainRebuildState::BuildingCpu;
    }

    return result;
}
} // namespace orbit::studio_session
