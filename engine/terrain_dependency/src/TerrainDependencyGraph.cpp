#include <orbit/terrain_dependency/TerrainDependencyGraph.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace orbit::terrain_dependency
{
namespace
{
constexpr u32 kMaximumBoundedRadiusTiles = 64U;

[[nodiscard]] std::size_t SourceIndex(
    const u8 source) noexcept
{
    return static_cast<std::size_t>(source);
}

[[nodiscard]] std::string PageNamePrefix(
    const terrain::PhysicalTerrainPageAddress& address)
{
    return
        "M27[" +
        std::to_string(
            static_cast<u32>(address.tile.face)) +
        ":" +
        std::to_string(address.tile.level) +
        ":" +
        std::to_string(address.tile.x) +
        ":" +
        std::to_string(address.tile.y) +
        "]";
}

[[nodiscard]] u32 EffectiveRadius(
    const TerrainSpatialInvalidationScope& scope) noexcept
{
    const u64 combined =
        static_cast<u64>(scope.radiusTiles) +
        static_cast<u64>(scope.downstreamRadiusTiles);

    return static_cast<u32>(
        std::min<u64>(
            combined,
            kMaximumBoundedRadiusTiles));
}

[[nodiscard]] u64 AddressFingerprint(
    const terrain::PhysicalTerrainPageAddress& address) noexcept
{
    u64 value = 0x4D32374144445253ULL;
    value = terrain::StableCombine64(value, address.planet.high);
    value = terrain::StableCombine64(value, address.planet.low);
    value = terrain::StableCombine64(
        value,
        static_cast<u64>(address.tile.face));
    value = terrain::StableCombine64(value, address.tile.level);
    value = terrain::StableCombine64(value, address.tile.x);
    value = terrain::StableCombine64(value, address.tile.y);
    return value;
}
} // namespace

bool TerrainSpatialInvalidationScope::IsValid() const noexcept
{
    if (!planet.IsValid())
    {
        return false;
    }

    if (global)
    {
        return true;
    }

    return
        radiusTiles <= kMaximumBoundedRadiusTiles &&
        downstreamRadiusTiles <= kMaximumBoundedRadiusTiles &&
        static_cast<u64>(radiusTiles) +
                static_cast<u64>(downstreamRadiusTiles) <=
            kMaximumBoundedRadiusTiles;
}

TerrainDependencyGraph::TerrainDependencyGraph(
    procedural_graph::ProceduralGraph& graph,
    TerrainPageBuildFunction build,
    terrain_gpu::PersistentGpuTerrainCache* cache)
    : graph_(graph),
      build_(std::move(build)),
      cache_(cache)
{
    if (!build_)
    {
        throw std::invalid_argument(
            "M27 terrain dependency graph requires a page build dispatcher.");
    }
}

TerrainDependencyGraph::TerrainDependencyGraph(
    procedural_graph::ProceduralGraph& graph,
    LegacyTerrainPageBuildFunction build,
    terrain_gpu::PersistentGpuTerrainCache* cache)
    : TerrainDependencyGraph(
          graph,
          [legacy = std::move(build)](
              const terrain::PhysicalTerrainPageAddress& address,
              const TerrainDependencyProduct product,
              const terrain::TerrainGenerationRevisions&,
              const procedural_graph::BuildContext& context)
          {
              return legacy(
                  address,
                  product,
                  context);
          },
          cache)
{
}

std::size_t TerrainDependencyGraph::AddressHash::operator()(
    const terrain::PhysicalTerrainPageAddress& address) const noexcept
{
    return static_cast<std::size_t>(
        AddressFingerprint(address));
}

void TerrainDependencyGraph::RegisterPage(
    const terrain::PhysicalTerrainPageAddress& address,
    const terrain::TerrainGenerationRevisions revisions)
{
    if (!address.planet.IsValid())
    {
        throw std::invalid_argument(
            "M27 cannot register an invalid physical terrain page address.");
    }

    if (pages_.contains(address))
    {
        throw std::invalid_argument(
            "M27 physical terrain page is already registered.");
    }

    const std::string prefix = PageNamePrefix(address);

    PageRecord record{};
    record.nodes.address = address;
    record.nodes.revisions = revisions;
    record.revisions =
        std::make_shared<RevisionState>();
    record.revisions->revisions =
        revisions;

    const auto addSource =
        [&](const SourceKind source,
            const char* suffix,
            const u64 revision)
        {
            record.sources[SourceIndex(
                static_cast<u8>(source))] =
                graph_.AddSource(
                    prefix + "." + suffix,
                    revision);
        };

    addSource(
        SourceKind::RockPhysics,
        "RockPhysics",
        revisions.geology);
    addSource(
        SourceKind::Authoring,
        "Authoring",
        revisions.authoring);
    addSource(
        SourceKind::Climate,
        "Climate",
        revisions.climate);
    addSource(
        SourceKind::Water,
        "Water",
        revisions.water);
    addSource(
        SourceKind::ProcessSettings,
        "ProcessSettings",
        revisions.processes);
    addSource(
        SourceKind::BiomePlacement,
        "BiomePlacement",
        revisions.biome);
    addSource(
        SourceKind::BiomeSurface,
        "BiomeSurface",
        revisions.biome);
    addSource(
        SourceKind::BiomeScatter,
        "BiomeScatter",
        revisions.biome);

    const auto sourceNode =
        [&](const SourceKind source)
        {
            return record.sources[
                SourceIndex(static_cast<u8>(source))];
        };

    const TerrainPageBuildFunction dispatcher = build_;
    const auto revisionState =
        record.revisions;

    const auto addProduct =
        [&](const TerrainDependencyProduct product,
            const char* suffix,
            std::vector<procedural_graph::NodeId> dependencies)
        {
            return graph_.AddDerived(
                prefix + "." + suffix,
                std::move(dependencies),
                BackendFor(product),
                [dispatcher,
                 revisionState,
                 address,
                 product](
                    const procedural_graph::BuildContext& context)
                {
                    terrain::TerrainGenerationRevisions revisions{};

                    {
                        std::scoped_lock lock(
                            revisionState->mutex);
                        revisions =
                            revisionState->revisions;
                    }

                    return dispatcher(
                        address,
                        product,
                        revisions,
                        context);
                });
        };

    record.nodes.geology =
        addProduct(
            TerrainDependencyProduct::Geology,
            "Geology",
            {
                sourceNode(SourceKind::RockPhysics),
                sourceNode(SourceKind::Authoring)
            });

    record.nodes.drainage =
        addProduct(
            TerrainDependencyProduct::Drainage,
            "Drainage",
            {
                record.nodes.geology,
                sourceNode(SourceKind::Climate),
                sourceNode(SourceKind::Authoring)
            });

    record.nodes.terrainProcesses =
        addProduct(
            TerrainDependencyProduct::TerrainProcesses,
            "TerrainProcesses",
            {
                record.nodes.geology,
                record.nodes.drainage,
                sourceNode(SourceKind::Climate),
                sourceNode(SourceKind::Water),
                sourceNode(SourceKind::ProcessSettings)
            });

    record.nodes.exposedSurface =
        addProduct(
            TerrainDependencyProduct::ExposedSurface,
            "ExposedSurface",
            {
                record.nodes.terrainProcesses
            });

    record.nodes.biomeWeights =
        addProduct(
            TerrainDependencyProduct::BiomeWeights,
            "BiomeWeights",
            {
                record.nodes.exposedSurface,
                sourceNode(SourceKind::Climate),
                sourceNode(SourceKind::BiomePlacement)
            });

    record.nodes.surfaceMaterial =
        addProduct(
            TerrainDependencyProduct::SurfaceMaterial,
            "SurfaceMaterial",
            {
                record.nodes.exposedSurface,
                record.nodes.biomeWeights,
                sourceNode(SourceKind::BiomeSurface)
            });

    record.nodes.scatter =
        addProduct(
            TerrainDependencyProduct::Scatter,
            "Scatter",
            {
                record.nodes.exposedSurface,
                record.nodes.biomeWeights,
                sourceNode(SourceKind::BiomeScatter)
            });

    pages_.emplace(
        address,
        std::move(record));
}

bool TerrainDependencyGraph::ContainsPage(
    const terrain::PhysicalTerrainPageAddress& address) const noexcept
{
    return pages_.contains(address);
}

bool TerrainDependencyGraph::UnregisterPage(
    const terrain::PhysicalTerrainPageAddress& address)
{
    const auto found =
        pages_.find(address);

    if (found == pages_.end())
    {
        return true;
    }

    const auto& page =
        found->second;

    std::array<
        procedural_graph::NodeId,
        kSourceCount +
            kTerrainDependencyProductCount>
        nodes{};

    std::size_t index = 0U;

    for (const auto source :
         page.sources)
    {
        nodes[index++] =
            source;
    }

    nodes[index++] =
        page.nodes.geology;
    nodes[index++] =
        page.nodes.drainage;
    nodes[index++] =
        page.nodes.terrainProcesses;
    nodes[index++] =
        page.nodes.exposedSurface;
    nodes[index++] =
        page.nodes.biomeWeights;
    nodes[index++] =
        page.nodes.surfaceMaterial;
    nodes[index++] =
        page.nodes.scatter;

    if (!graph_.RemoveNodes(
            nodes))
    {
        return false;
    }

    if (cache_ != nullptr)
    {
        static_cast<void>(
            cache_->InvalidateAddress(
                address));
    }

    pages_.erase(found);
    return true;
}

void TerrainDependencyGraph::RequestBuild(
    const terrain::PhysicalTerrainPageAddress& address,
    const TerrainDependencyProduct product)
{
    const auto found = pages_.find(address);
    if (found == pages_.end())
    {
        throw std::invalid_argument(
            "M27 cannot request a build for an unregistered terrain page.");
    }

    graph_.RequestBuild(
        ProductNode(found->second, product));
}

void TerrainDependencyGraph::Poll()
{
    graph_.Poll();
}

procedural_graph::ExecutionBackend
TerrainDependencyGraph::Backend(
    const TerrainDependencyProduct product) noexcept
{
    return BackendFor(product);
}

TerrainDependencyProductMask
TerrainDependencyGraph::ProductsForChange(
    const TerrainChangeKind kind) noexcept
{
    return DirtyProductsFor(kind);
}

TerrainDependencyGraph::SourceKind
TerrainDependencyGraph::SourceFor(
    const TerrainChangeKind kind) noexcept
{
    switch (kind)
    {
    case TerrainChangeKind::RockPhysics:
        return SourceKind::RockPhysics;
    case TerrainChangeKind::TerrainAuthoring:
        return SourceKind::Authoring;
    case TerrainChangeKind::Climate:
        return SourceKind::Climate;
    case TerrainChangeKind::Water:
        return SourceKind::Water;
    case TerrainChangeKind::ProcessSettings:
        return SourceKind::ProcessSettings;
    case TerrainChangeKind::BiomePlacement:
        return SourceKind::BiomePlacement;
    case TerrainChangeKind::BiomeSurfaceMaterial:
        return SourceKind::BiomeSurface;
    case TerrainChangeKind::BiomeScatter:
        return SourceKind::BiomeScatter;
    }

    return SourceKind::Authoring;
}

TerrainDependencyProductMask
TerrainDependencyGraph::DirtyProductsFor(
    const TerrainChangeKind kind) noexcept
{
    const auto bit =
        [](const TerrainDependencyProduct product)
        {
            return ProductBit(product);
        };

    switch (kind)
    {
    case TerrainChangeKind::RockPhysics:
    case TerrainChangeKind::TerrainAuthoring:
        return
            bit(TerrainDependencyProduct::Geology) |
            bit(TerrainDependencyProduct::Drainage) |
            bit(TerrainDependencyProduct::TerrainProcesses) |
            bit(TerrainDependencyProduct::ExposedSurface) |
            bit(TerrainDependencyProduct::BiomeWeights) |
            bit(TerrainDependencyProduct::SurfaceMaterial) |
            bit(TerrainDependencyProduct::Scatter);

    case TerrainChangeKind::Climate:
        return
            bit(TerrainDependencyProduct::Drainage) |
            bit(TerrainDependencyProduct::TerrainProcesses) |
            bit(TerrainDependencyProduct::ExposedSurface) |
            bit(TerrainDependencyProduct::BiomeWeights) |
            bit(TerrainDependencyProduct::SurfaceMaterial) |
            bit(TerrainDependencyProduct::Scatter);

    case TerrainChangeKind::Water:
    case TerrainChangeKind::ProcessSettings:
        return
            bit(TerrainDependencyProduct::TerrainProcesses) |
            bit(TerrainDependencyProduct::ExposedSurface) |
            bit(TerrainDependencyProduct::BiomeWeights) |
            bit(TerrainDependencyProduct::SurfaceMaterial) |
            bit(TerrainDependencyProduct::Scatter);

    case TerrainChangeKind::BiomePlacement:
        return
            bit(TerrainDependencyProduct::BiomeWeights) |
            bit(TerrainDependencyProduct::SurfaceMaterial) |
            bit(TerrainDependencyProduct::Scatter);

    case TerrainChangeKind::BiomeSurfaceMaterial:
        return
            bit(TerrainDependencyProduct::SurfaceMaterial);

    case TerrainChangeKind::BiomeScatter:
        return
            bit(TerrainDependencyProduct::Scatter);
    }

    return 0;
}

procedural_graph::ExecutionBackend
TerrainDependencyGraph::BackendFor(
    const TerrainDependencyProduct product) noexcept
{
    switch (product)
    {
    case TerrainDependencyProduct::Drainage:
    case TerrainDependencyProduct::TerrainProcesses:
    case TerrainDependencyProduct::Scatter:
        return procedural_graph::ExecutionBackend::Gpu;

    case TerrainDependencyProduct::Geology:
    case TerrainDependencyProduct::ExposedSurface:
    case TerrainDependencyProduct::BiomeWeights:
    case TerrainDependencyProduct::SurfaceMaterial:
    case TerrainDependencyProduct::Count:
        return procedural_graph::ExecutionBackend::Cpu;
    }

    return procedural_graph::ExecutionBackend::Cpu;
}

bool TerrainDependencyGraph::AddressInScope(
    const terrain::PhysicalTerrainPageAddress& address,
    const TerrainSpatialInvalidationScope& scope)
{
    if (address.planet != scope.planet)
    {
        return false;
    }

    if (scope.global)
    {
        return true;
    }

    if (address.tile.level != scope.center.level)
    {
        return false;
    }

    const auto neighborhood =
        world::TileNeighborhood(
            scope.center,
            EffectiveRadius(scope));

    return std::find(
               neighborhood.begin(),
               neighborhood.end(),
               address.tile) !=
        neighborhood.end();
}

void TerrainDependencyGraph::IncrementRevision(
    terrain::TerrainGenerationRevisions& revisions,
    const TerrainChangeKind kind) noexcept
{
    switch (kind)
    {
    case TerrainChangeKind::RockPhysics:
        ++revisions.geology;
        break;
    case TerrainChangeKind::TerrainAuthoring:
        ++revisions.authoring;
        break;
    case TerrainChangeKind::Climate:
        ++revisions.climate;
        break;
    case TerrainChangeKind::Water:
        ++revisions.water;
        break;
    case TerrainChangeKind::ProcessSettings:
        ++revisions.processes;
        break;
    case TerrainChangeKind::BiomePlacement:
    case TerrainChangeKind::BiomeSurfaceMaterial:
    case TerrainChangeKind::BiomeScatter:
        ++revisions.biome;
        break;
    }
}

TerrainInvalidationResult TerrainDependencyGraph::ApplyChange(
    const TerrainInvalidationRequest& request)
{
    if (!request.scope.IsValid())
    {
        throw std::invalid_argument(
            "M27 invalid terrain spatial invalidation scope.");
    }

    TerrainInvalidationResult result{
        .dirtyProducts =
            DirtyProductsFor(request.kind)
    };

    const SourceKind source =
        SourceFor(request.kind);

    for (auto& [address, page] : pages_)
    {
        if (!AddressInScope(address, request.scope))
        {
            continue;
        }

        graph_.Invalidate(
            page.sources[
                SourceIndex(static_cast<u8>(source))]);

        IncrementRevision(
            page.nodes.revisions,
            request.kind);

        {
            std::scoped_lock lock(
                page.revisions->mutex);
            page.revisions->revisions =
                page.nodes.revisions;
        }

        if (cache_ != nullptr)
        {
            result.cacheEntriesRemoved +=
                cache_->InvalidateAddress(address);
        }

        ++result.affectedPages;
    }

    return result;
}

procedural_graph::NodeId TerrainDependencyGraph::ProductNode(
    const PageRecord& page,
    const TerrainDependencyProduct product) const
{
    switch (product)
    {
    case TerrainDependencyProduct::Geology:
        return page.nodes.geology;
    case TerrainDependencyProduct::Drainage:
        return page.nodes.drainage;
    case TerrainDependencyProduct::TerrainProcesses:
        return page.nodes.terrainProcesses;
    case TerrainDependencyProduct::ExposedSurface:
        return page.nodes.exposedSurface;
    case TerrainDependencyProduct::BiomeWeights:
        return page.nodes.biomeWeights;
    case TerrainDependencyProduct::SurfaceMaterial:
        return page.nodes.surfaceMaterial;
    case TerrainDependencyProduct::Scatter:
        return page.nodes.scatter;
    case TerrainDependencyProduct::Count:
        break;
    }

    throw std::invalid_argument(
        "M27 invalid terrain dependency product.");
}

bool TerrainDependencyGraph::BuildBlocking(
    const terrain::PhysicalTerrainPageAddress& address,
    const TerrainDependencyProduct product)
{
    const auto found = pages_.find(address);
    if (found == pages_.end())
    {
        throw std::invalid_argument(
            "M27 cannot build an unregistered terrain page.");
    }

    return graph_.BuildBlocking(
        ProductNode(found->second, product));
}

std::optional<procedural_graph::NodeStatus>
TerrainDependencyGraph::Status(
    const terrain::PhysicalTerrainPageAddress& address,
    const TerrainDependencyProduct product) const
{
    const auto found = pages_.find(address);
    if (found == pages_.end())
    {
        return std::nullopt;
    }

    return graph_.Status(
        ProductNode(found->second, product));
}

std::optional<terrain::TerrainGenerationRevisions>
TerrainDependencyGraph::Revisions(
    const terrain::PhysicalTerrainPageAddress& address) const
{
    const auto found = pages_.find(address);
    if (found == pages_.end())
    {
        return std::nullopt;
    }

    std::scoped_lock lock(
        found->second.revisions->mutex);
    return found->second.revisions->revisions;
}

std::optional<TerrainDependencyPageNodes>
TerrainDependencyGraph::Nodes(
    const terrain::PhysicalTerrainPageAddress& address) const
{
    const auto found = pages_.find(address);
    if (found == pages_.end())
    {
        return std::nullopt;
    }

    auto result =
        found->second.nodes;

    {
        std::scoped_lock lock(
            found->second.revisions->mutex);
        result.revisions =
            found->second.revisions->revisions;
    }

    return result;
}
} // namespace orbit::terrain_dependency
