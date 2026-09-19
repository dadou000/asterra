#pragma once

#include <orbit/terrain_debug/TerrainDebugPageData.hpp>

#include <memory>
#include <shared_mutex>
#include <span>
#include <unordered_map>

namespace orbit::terrain_debug
{
// One immutable diagnostic capture assembled from the actual products owned by
// a physical terrain page. Every pointer/span is optional; absent products stay
// absent instead of being synthesized by the debug layer.
struct TerrainDebugLivePageInputs
{
    terrain::PhysicalTerrainPageAddress address{};
    u8 physicalLod{0};
    terrain::TerrainGenerationRevisions revisions{};
    bool cacheResident{false};
    u64 invalidationRevision{0};

    u32 width{0};
    u32 height{0};

    const terrain_material_column::MaterialColumnPage* materialColumn{nullptr};
    const terrain_hydrology::DrainagePage* drainage{nullptr};
    const terrain_erosion::HydraulicErosionResult* hydraulic{nullptr};
    const terrain_erosion::SedimentExchangePage* sedimentExchange{nullptr};

    std::span<const terrain_macro_geology::MacroGeologySample>
        macroGeology{};
    std::span<const terrain_geology::StratigraphySample>
        stratigraphy{};

    std::span<const terrain_erosion::AeolianCellForcing>
        aeolianForcing{};
    const terrain_erosion::AeolianErosionResult* aeolian{nullptr};

    std::span<const f32> dominantBiomeWeights{};
    std::span<const terrain_biome::BiomeId> dominantBiomes{};

    const terrain_scatter::ScatterPageRequest* scatterRequest{nullptr};
    std::span<const terrain_scatter::DerivedScatterInstance>
        scatterInstances{};
};

[[nodiscard]] std::shared_ptr<const TerrainDebugPageData>
CaptureLiveTerrainDebugPage(
    const TerrainDebugLivePageInputs& inputs);

// Project/session-scoped, derived-only lookup of the currently published
// physical terrain debug pages. Producers publish immutable snapshots after
// their real page products change. Studio reads by stable physical address.
// This registry is not terrain authority and never generates terrain itself.
class TerrainDebugLivePages
{
public:
    void Publish(
        std::shared_ptr<const TerrainDebugPageData> page);

    [[nodiscard]] std::shared_ptr<const TerrainDebugPageData>
    Find(
        const terrain::PhysicalTerrainPageAddress& address) const;

    [[nodiscard]] bool Erase(
        const terrain::PhysicalTerrainPageAddress& address);

    void Clear();

    [[nodiscard]] std::size_t Size() const;

private:
    struct AddressHash
    {
        [[nodiscard]] std::size_t operator()(
            const terrain::PhysicalTerrainPageAddress& address) const noexcept;
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<
        terrain::PhysicalTerrainPageAddress,
        std::shared_ptr<const TerrainDebugPageData>,
        AddressHash> pages_;
};
} // namespace orbit::terrain_debug
