#pragma once

#include <orbit/terrain_debug/TerrainDebugField.hpp>
#include <orbit/terrain_debug/TerrainDebugRaster.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_erosion/AeolianErosion.hpp>
#include <orbit/terrain_erosion/HydraulicErosion.hpp>
#include <orbit/terrain_erosion/SedimentExchange.hpp>
#include <orbit/terrain_geology/Stratigraphy.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>
#include <orbit/terrain_macro_geology/MacroGeologyField.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/terrain_scatter/DeterministicScatter.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace orbit::terrain_debug
{
// Owns derived debug samples for one physical page. The class copies from
// canonical/derived source products so spans returned by View() remain stable;
// it never mutates or replaces the source terrain products.
class TerrainDebugPageData
{
public:
    TerrainDebugPageData(
        TerrainDebugPageStamp stamp,
        u32 width,
        u32 height);

    [[nodiscard]] const TerrainDebugPageStamp& Stamp() const noexcept;
    [[nodiscard]] u32 Width() const noexcept;
    [[nodiscard]] u32 Height() const noexcept;

    // Direct adapters for existing V0.0.4 products.
    void CaptureMaterialColumn(
        const terrain_material_column::MaterialColumnPage& page);

    void CaptureDrainage(
        const terrain_hydrology::DrainagePage& page);

    void CaptureHydraulic(
        const terrain_erosion::HydraulicErosionResult& result);

    void CaptureSedimentExchange(
        const terrain_erosion::SedimentExchangePage& page);

    void CaptureMacroGeology(
        std::span<const terrain_macro_geology::MacroGeologySample> samples);

    void CaptureStratigraphy(
        std::span<const terrain_geology::StratigraphySample> samples);

    void CaptureAeolian(
        std::span<const terrain_erosion::AeolianCellForcing> forcing,
        const terrain_erosion::AeolianErosionResult& result);

    void CaptureBiomeResolution(
        std::span<const f32> dominantWeights,
        std::span<const terrain_biome::BiomeId> dominantBiomes);

    void CaptureScatterDensity(
        const terrain_scatter::ScatterPageRequest& request,
        std::span<const terrain_scatter::DerivedScatterInstance> instances);

    // Explicit bindings for fields whose canonical producers live outside the
    // page types above (geology, climate/wind, biome resolution, M14 boundary
    // flux, scatter density). Value-class validation prevents accidental
    // reinterpretation of one field as another.
    void SetScalar(
        TerrainDebugField field,
        std::span<const f32> values);

    void SetVector(
        TerrainDebugField field,
        std::span<const TerrainDebugVector2> values);

    void SetCategory(
        TerrainDebugField field,
        std::span<const u32> values);

    // Uniform page-provenance views are generated from the M29 stamp.
    void CapturePageProvenance();

    [[nodiscard]] bool Has(
        TerrainDebugField field) const noexcept;

    [[nodiscard]] TerrainDebugRasterView View(
        TerrainDebugField field,
        TerrainDebugRasterRange range = {}) const;

private:
    struct FieldStorage
    {
        std::vector<f32> scalar;
        std::vector<TerrainDebugVector2> vector;
        std::vector<u32> category;
        std::vector<u8> boolean;
        std::vector<u64> revision;
        std::vector<u8> lod;

        [[nodiscard]] bool Empty() const noexcept;
        void Clear();
    };

    [[nodiscard]] std::size_t Index(
        TerrainDebugField field) const;

    [[nodiscard]] u64 TexelCount() const noexcept;
    void RequireTexelCount(
        std::size_t size,
        const char* source) const;

    TerrainDebugPageStamp stamp_{};
    u32 width_{0};
    u32 height_{0};

    std::array<
        FieldStorage,
        static_cast<std::size_t>(TerrainDebugField::Count)>
        fields_{};
};
} // namespace orbit::terrain_debug
