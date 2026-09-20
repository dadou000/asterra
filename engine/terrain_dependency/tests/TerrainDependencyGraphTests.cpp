#include <orbit/terrain_dependency/TerrainDependencyGraph.hpp>

#include <orbit/jobs/JobSystem.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace
{
using namespace orbit;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M27 failure: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        Fail(message);
    }
}

class FakeBuffer final : public rhi::Buffer
{
public:
    explicit FakeBuffer(const u64 bytes)
        : bytes_(bytes)
    {
    }

    [[nodiscard]] u64 SizeBytes() const noexcept override { return bytes_; }
    [[nodiscard]] rhi::BufferUsage Usage() const noexcept override
    {
        return rhi::BufferUsage::Structured;
    }
    [[nodiscard]] rhi::MemoryUsage Memory() const noexcept override
    {
        return rhi::MemoryUsage::GpuOnly;
    }
    [[nodiscard]] std::byte* Map() override { return nullptr; }
    void Unmap() override {}

private:
    u64 bytes_{0};
};

terrain::PhysicalTerrainPageAddress MakeAddress(
    const world::PlanetTileId tile)
{
    return {
        .planet = {
            .high = 0x4F524249544D3237ULL,
            .low = 0x0000000000000001ULL
        },
        .tile = tile
    };
}

terrain::TerrainGenerationRevisions InitialRevisions()
{
    return {
        .geology = 10,
        .climate = 20,
        .authoring = 30,
        .biome = 40,
        .water = 50,
        .processes = 60
    };
}

std::size_t ProductIndex(
    const terrain_dependency::TerrainDependencyProduct product)
{
    return static_cast<std::size_t>(product);
}

struct Fixture
{
    jobs::JobSystem jobs{2};
    procedural_graph::ProceduralGraph graph{jobs};

    std::array<
        std::atomic<u64>,
        terrain_dependency::kTerrainDependencyProductCount> counts{};

    terrain_gpu::PersistentGpuTerrainCache cache{{
        .maximumResidentBytes = 4ULL * 1024ULL * 1024ULL,
        .maximumPages = 128
    }};

    terrain_dependency::TerrainDependencyGraph dependencies{
        graph,
        [this](
            const terrain::PhysicalTerrainPageAddress&,
            const terrain_dependency::TerrainDependencyProduct product,
            const procedural_graph::BuildContext&)
        {
            ++counts[ProductIndex(product)];
            return std::any(
                static_cast<u32>(product));
        },
        &cache
    };

    [[nodiscard]] u64 Count(
        const terrain_dependency::TerrainDependencyProduct product) const
    {
        return counts[ProductIndex(product)].load();
    }
};

void BuildMaterialAndScatter(
    Fixture& fixture,
    const terrain::PhysicalTerrainPageAddress& address)
{
    Require(
        fixture.dependencies.BuildBlocking(
            address,
            terrain_dependency::TerrainDependencyProduct::SurfaceMaterial),
        "Surface material build must succeed.");

    Require(
        fixture.dependencies.BuildBlocking(
            address,
            terrain_dependency::TerrainDependencyProduct::Scatter),
        "Scatter build must succeed.");
}

terrain_dependency::TerrainInvalidationRequest GlobalChange(
    const terrain_dependency::TerrainChangeKind kind,
    const world::PlanetId planet)
{
    return {
        .kind = kind,
        .scope = {
            .planet = planet,
            .global = true
        }
    };
}

void TestBiomeTreeDensityInvalidatesScatterOnly()
{
    Fixture fixture;

    const auto address = MakeAddress({
        .face = world::CubeFace::PositiveX,
        .level = 6,
        .x = 20,
        .y = 21
    });

    fixture.dependencies.RegisterPage(
        address,
        InitialRevisions());

    BuildMaterialAndScatter(fixture, address);

    const u64 processBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::TerrainProcesses);
    const u64 surfaceBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::SurfaceMaterial);
    const u64 scatterBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Scatter);

    const auto result =
        fixture.dependencies.ApplyChange(
            GlobalChange(
                terrain_dependency::TerrainChangeKind::BiomeScatter,
                address.planet));

    Require(
        result.affectedPages == 1 &&
        result.dirtyProducts ==
            terrain_dependency::ProductBit(
                terrain_dependency::TerrainDependencyProduct::Scatter),
        "Tree-density edit must dirty scatter only.");

    Require(
        fixture.dependencies.BuildBlocking(
            address,
            terrain_dependency::TerrainDependencyProduct::Scatter),
        "Scatter rebuild must succeed.");

    Require(
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::TerrainProcesses) ==
            processBefore,
        "Tree-density edit must not rerun erosion/process terrain.");

    Require(
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::SurfaceMaterial) ==
            surfaceBefore,
        "Tree-density edit must not rebuild surface material.");

    Require(
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Scatter) ==
            scatterBefore + 1,
        "Tree-density edit must rebuild scatter exactly once.");
}

void TestMossMaterialInvalidatesSurfaceOnly()
{
    Fixture fixture;

    const auto address = MakeAddress({
        .face = world::CubeFace::PositiveY,
        .level = 6,
        .x = 15,
        .y = 17
    });

    fixture.dependencies.RegisterPage(
        address,
        InitialRevisions());

    BuildMaterialAndScatter(fixture, address);

    const u64 processBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::TerrainProcesses);
    const u64 scatterBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Scatter);
    const u64 surfaceBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::SurfaceMaterial);

    const auto result =
        fixture.dependencies.ApplyChange(
            GlobalChange(
                terrain_dependency::TerrainChangeKind::BiomeSurfaceMaterial,
                address.planet));

    Require(
        result.dirtyProducts ==
            terrain_dependency::ProductBit(
                terrain_dependency::TerrainDependencyProduct::SurfaceMaterial),
        "Moss material edit must dirty only the surface-material node.");

    Require(
        fixture.dependencies.BuildBlocking(
            address,
            terrain_dependency::TerrainDependencyProduct::SurfaceMaterial),
        "Surface material rebuild must succeed.");

    Require(
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::TerrainProcesses) ==
            processBefore &&
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Scatter) ==
            scatterBefore &&
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::SurfaceMaterial) ==
            surfaceBefore + 1,
        "Moss edit must not rerun erosion or scatter.");
}

void TestRockErodibilityInvalidatesProcessDescendants()
{
    Fixture fixture;

    const auto address = MakeAddress({
        .face = world::CubeFace::NegativeZ,
        .level = 6,
        .x = 18,
        .y = 19
    });

    fixture.dependencies.RegisterPage(
        address,
        InitialRevisions());

    BuildMaterialAndScatter(fixture, address);

    const u64 geologyBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Geology);
    const u64 processBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::TerrainProcesses);
    const u64 scatterBefore =
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Scatter);

    static_cast<void>(
        fixture.dependencies.ApplyChange(
            GlobalChange(
                terrain_dependency::TerrainChangeKind::RockPhysics,
                address.planet)));

    Require(
        fixture.dependencies.BuildBlocking(
            address,
            terrain_dependency::TerrainDependencyProduct::Scatter),
        "Rock-dependent scatter rebuild chain must succeed.");

    Require(
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Geology) ==
            geologyBefore + 1 &&
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::TerrainProcesses) ==
            processBefore + 1 &&
        fixture.Count(
            terrain_dependency::TerrainDependencyProduct::Scatter) ==
            scatterBefore + 1,
        "Rock erodibility must rerun geology, erosion/process descendants and scatter.");
}

void TestProcessSettingsInvalidatesOnlyProcessDescendants()
{
    Fixture fixture;

    const auto address = MakeAddress({
        .face = world::CubeFace::NegativeY,
        .level = 6,
        .x = 11,
        .y = 13
    });

    const auto initial =
        InitialRevisions();

    fixture.dependencies.RegisterPage(
        address,
        initial);

    BuildMaterialAndScatter(
        fixture,
        address);

    const u64 geologyBefore =
        fixture.Count(
            terrain_dependency::
                TerrainDependencyProduct::
                    Geology);
    const u64 processBefore =
        fixture.Count(
            terrain_dependency::
                TerrainDependencyProduct::
                    TerrainProcesses);
    const u64 scatterBefore =
        fixture.Count(
            terrain_dependency::
                TerrainDependencyProduct::
                    Scatter);

    const auto result =
        fixture.dependencies.ApplyChange(
            GlobalChange(
                terrain_dependency::
                    TerrainChangeKind::
                        ProcessSettings,
                address.planet));

    const auto expectedMask =
        terrain_dependency::ProductBit(
            terrain_dependency::
                TerrainDependencyProduct::
                    TerrainProcesses) |
        terrain_dependency::ProductBit(
            terrain_dependency::
                TerrainDependencyProduct::
                    ExposedSurface) |
        terrain_dependency::ProductBit(
            terrain_dependency::
                TerrainDependencyProduct::
                    BiomeWeights) |
        terrain_dependency::ProductBit(
            terrain_dependency::
                TerrainDependencyProduct::
                    SurfaceMaterial) |
        terrain_dependency::ProductBit(
            terrain_dependency::
                TerrainDependencyProduct::
                    Scatter);

    Require(
        result.affectedPages == 1 &&
        result.dirtyProducts == expectedMask,
        "Process settings must dirty only process products and descendants.");

    const auto revisions =
        fixture.dependencies.Revisions(
            address);

    Require(
        revisions.has_value() &&
        revisions->processes ==
            initial.processes + 1U &&
        revisions->geology ==
            initial.geology &&
        revisions->climate ==
            initial.climate &&
        revisions->authoring ==
            initial.authoring &&
        revisions->biome ==
            initial.biome &&
        revisions->water ==
            initial.water,
        "Process settings must increment only the processes revision domain.");

    Require(
        fixture.dependencies.BuildBlocking(
            address,
            terrain_dependency::
                TerrainDependencyProduct::
                    Scatter),
        "Process settings scatter descendant rebuild must succeed.");

    Require(
        fixture.Count(
            terrain_dependency::
                TerrainDependencyProduct::
                    Geology) ==
            geologyBefore,
        "Aeolian/hydraulic process edits must never rebuild geology.");

    Require(
        fixture.Count(
            terrain_dependency::
                TerrainDependencyProduct::
                    TerrainProcesses) ==
            processBefore + 1U,
        "Process settings must rebuild the process stage.");

    Require(
        fixture.Count(
            terrain_dependency::
                TerrainDependencyProduct::
                    Scatter) ==
            scatterBefore + 1U,
        "Process settings must rebuild downstream scatter once.");
}

void TestSpatialInvalidationIsBounded()
{
    Fixture fixture;

    const world::PlanetTileId centerTile{
        .face = world::CubeFace::PositiveX,
        .level = 6,
        .x = 24,
        .y = 24
    };

    const auto center = MakeAddress(centerTile);
    const auto neighbor =
        MakeAddress(
            world::OffsetTile(centerTile, 1, 0));
    const auto far =
        MakeAddress(
            world::OffsetTile(centerTile, 3, 0));

    fixture.dependencies.RegisterPage(center, InitialRevisions());
    fixture.dependencies.RegisterPage(neighbor, InitialRevisions());
    fixture.dependencies.RegisterPage(far, InitialRevisions());

    BuildMaterialAndScatter(fixture, center);
    BuildMaterialAndScatter(fixture, neighbor);
    BuildMaterialAndScatter(fixture, far);

    const auto farBefore =
        fixture.dependencies.Status(
            far,
            terrain_dependency::TerrainDependencyProduct::Scatter);

    Require(farBefore.has_value(), "Far page status must exist.");

    const auto result =
        fixture.dependencies.ApplyChange({
            .kind =
                terrain_dependency::TerrainChangeKind::TerrainAuthoring,
            .scope = {
                .planet = center.planet,
                .global = false,
                .center = centerTile,
                .radiusTiles = 0,
                .downstreamRadiusTiles = 1
            }
        });

    Require(
        result.affectedPages == 2,
        "One-tile downstream authoring invalidation must affect center plus registered neighbor only.");

    BuildMaterialAndScatter(fixture, center);
    BuildMaterialAndScatter(fixture, neighbor);

    const auto farAfter =
        fixture.dependencies.Status(
            far,
            terrain_dependency::TerrainDependencyProduct::Scatter);

    Require(
        farAfter.has_value() &&
        farAfter->state == procedural_graph::NodeState::Clean &&
        farAfter->committedRevision == farBefore->committedRevision,
        "Spatially distant terrain must remain clean and keep its committed product.");
}

void TestRevisionsAndPersistentCacheAreInvalidatedTogether()
{
    Fixture fixture;

    const auto address = MakeAddress({
        .face = world::CubeFace::PositiveZ,
        .level = 6,
        .x = 9,
        .y = 12
    });

    const auto initial = InitialRevisions();

    fixture.dependencies.RegisterPage(address, initial);

    auto cached =
        std::make_shared<terrain_gpu::CachedGpuTerrainPage>();
    cached->products =
        terrain_gpu::ProductBit(
            terrain_gpu::CachedTerrainProduct::Scatter);
    cached->buffers.push_back(
        std::make_shared<FakeBuffer>(256));

    fixture.cache.Insert(
        {
            .address = address,
            .physicalLod = 2,
            .revisions = initial
        },
        cached);

    const auto result =
        fixture.dependencies.ApplyChange(
            GlobalChange(
                terrain_dependency::TerrainChangeKind::BiomeScatter,
                address.planet));

    const auto revisions =
        fixture.dependencies.Revisions(address);

    Require(
        revisions.has_value() &&
        revisions->biome == initial.biome + 1 &&
        revisions->geology == initial.geology &&
        revisions->processes == initial.processes,
        "Scatter edit must advance only the biome revision domain.");

    Require(
        result.cacheEntriesRemoved == 1 &&
        fixture.cache.Stats().residentPages == 0,
        "Affected physical page cache entries must be invalidated with the graph edit.");
}
} // namespace

int main()
{
    TestBiomeTreeDensityInvalidatesScatterOnly();
    TestMossMaterialInvalidatesSurfaceOnly();
    TestRockErodibilityInvalidatesProcessDescendants();
    TestProcessSettingsInvalidatesOnlyProcessDescendants();
    TestSpatialInvalidationIsBounded();
    TestRevisionsAndPersistentCacheAreInvalidatedTogether();

    std::cout << "Orbit M27 terrain dependency invalidation tests passed.\n";
    return EXIT_SUCCESS;
}
