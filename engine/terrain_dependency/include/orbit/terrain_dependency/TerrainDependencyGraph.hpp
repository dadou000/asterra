#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/procedural_graph/ProceduralGraph.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_gpu/PersistentGpuTerrainCache.hpp>
#include <orbit/world/PlanetTileNeighborhood.hpp>

#include <any>
#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace orbit::terrain_dependency
{
enum class TerrainDependencyProduct : u8
{
    Geology = 0,
    Drainage,
    TerrainProcesses,
    ExposedSurface,
    BiomeWeights,
    SurfaceMaterial,
    Scatter,
    Count
};

inline constexpr std::size_t kTerrainDependencyProductCount =
    static_cast<std::size_t>(TerrainDependencyProduct::Count);

using TerrainDependencyProductMask = u32;

[[nodiscard]] constexpr TerrainDependencyProductMask ProductBit(
    const TerrainDependencyProduct product) noexcept
{
    return 1U << static_cast<u32>(product);
}

enum class TerrainChangeKind : u8
{
    RockPhysics = 0,
    TerrainAuthoring,
    Climate,
    Water,
    ProcessSettings,
    BiomePlacement,
    BiomeSurfaceMaterial,
    BiomeScatter
};

struct TerrainSpatialInvalidationScope
{
    world::PlanetId planet{};

    bool global{true};

    // Used only when global=false. Bounded scopes operate at this physical
    // page tile level. Cross-level edits should be projected by the caller or
    // expressed as global until M27 gains a hierarchical page index.
    world::PlanetTileId center{};

    u32 radiusTiles{0};

    // Additional bounded downstream reach for effects such as a canyon edit
    // changing neighboring drainage/process descendants.
    u32 downstreamRadiusTiles{0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct TerrainInvalidationRequest
{
    TerrainChangeKind kind{TerrainChangeKind::TerrainAuthoring};
    TerrainSpatialInvalidationScope scope{};
};

struct TerrainInvalidationResult
{
    u64 affectedPages{0};
    u64 cacheEntriesRemoved{0};
    TerrainDependencyProductMask dirtyProducts{0};
};

using TerrainPageBuildFunction =
    std::function<std::any(
        const terrain::PhysicalTerrainPageAddress&,
        TerrainDependencyProduct,
        const procedural_graph::BuildContext&)>;

struct TerrainDependencyPageNodes
{
    terrain::PhysicalTerrainPageAddress address{};

    procedural_graph::NodeId geology{};
    procedural_graph::NodeId drainage{};
    procedural_graph::NodeId terrainProcesses{};
    procedural_graph::NodeId exposedSurface{};
    procedural_graph::NodeId biomeWeights{};
    procedural_graph::NodeId surfaceMaterial{};
    procedural_graph::NodeId scatter{};

    terrain::TerrainGenerationRevisions revisions{};
};

class TerrainDependencyGraph
{
public:
    TerrainDependencyGraph(
        procedural_graph::ProceduralGraph& graph,
        TerrainPageBuildFunction build,
        terrain_gpu::PersistentGpuTerrainCache* cache = nullptr);

    void RegisterPage(
        const terrain::PhysicalTerrainPageAddress& address,
        terrain::TerrainGenerationRevisions revisions = {});

    [[nodiscard]] bool ContainsPage(
        const terrain::PhysicalTerrainPageAddress& address) const noexcept;

    // Non-blocking build seam used by editor/runtime schedulers. The
    // underlying ProceduralGraph still owns dependency ordering and stale
    // generation rejection.
    void RequestBuild(
        const terrain::PhysicalTerrainPageAddress& address,
        TerrainDependencyProduct product);

    void Poll();

    [[nodiscard]] static procedural_graph::ExecutionBackend Backend(
        TerrainDependencyProduct product) noexcept;

    [[nodiscard]] static TerrainDependencyProductMask ProductsForChange(
        TerrainChangeKind kind) noexcept;

    [[nodiscard]] TerrainInvalidationResult ApplyChange(
        const TerrainInvalidationRequest& request);

    [[nodiscard]] bool BuildBlocking(
        const terrain::PhysicalTerrainPageAddress& address,
        TerrainDependencyProduct product);

    [[nodiscard]] std::optional<procedural_graph::NodeStatus> Status(
        const terrain::PhysicalTerrainPageAddress& address,
        TerrainDependencyProduct product) const;

    [[nodiscard]] std::optional<terrain::TerrainGenerationRevisions>
    Revisions(
        const terrain::PhysicalTerrainPageAddress& address) const;

    [[nodiscard]] std::optional<TerrainDependencyPageNodes>
    Nodes(
        const terrain::PhysicalTerrainPageAddress& address) const;

private:
    enum class SourceKind : u8
    {
        RockPhysics = 0,
        Authoring,
        Climate,
        Water,
        ProcessSettings,
        BiomePlacement,
        BiomeSurface,
        BiomeScatter,
        Count
    };

    static constexpr std::size_t kSourceCount =
        static_cast<std::size_t>(SourceKind::Count);

    struct PageRecord
    {
        TerrainDependencyPageNodes nodes{};
        std::array<procedural_graph::NodeId, kSourceCount> sources{};
    };

    struct AddressHash
    {
        [[nodiscard]] std::size_t operator()(
            const terrain::PhysicalTerrainPageAddress& address) const noexcept;
    };

    [[nodiscard]] static SourceKind SourceFor(
        TerrainChangeKind kind) noexcept;

    [[nodiscard]] static TerrainDependencyProductMask DirtyProductsFor(
        TerrainChangeKind kind) noexcept;

    [[nodiscard]] static procedural_graph::ExecutionBackend BackendFor(
        TerrainDependencyProduct product) noexcept;

    [[nodiscard]] static bool AddressInScope(
        const terrain::PhysicalTerrainPageAddress& address,
        const TerrainSpatialInvalidationScope& scope);

    static void IncrementRevision(
        terrain::TerrainGenerationRevisions& revisions,
        TerrainChangeKind kind) noexcept;

    [[nodiscard]] procedural_graph::NodeId ProductNode(
        const PageRecord& page,
        TerrainDependencyProduct product) const;

    procedural_graph::ProceduralGraph& graph_;
    TerrainPageBuildFunction build_;
    terrain_gpu::PersistentGpuTerrainCache* cache_{nullptr};

    std::unordered_map<
        terrain::PhysicalTerrainPageAddress,
        PageRecord,
        AddressHash> pages_;
};
} // namespace orbit::terrain_dependency
