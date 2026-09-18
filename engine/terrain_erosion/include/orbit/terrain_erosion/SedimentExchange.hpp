#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace orbit::terrain_erosion
{
enum class SedimentClass : u8
{
    Sand,
    Fines,
    CoarseDebris
};

enum class SedimentTransportMedium : u8
{
    Waterborne,
    Airborne,
    SurfaceMobile
};

enum class SedimentSourceProcess : u8
{
    Hydraulic,
    AeolianAbrasion,
    ThermalFracture,
    GlacialErosion,
    CoastalErosion
};

struct SedimentMass
{
    f64 sandKg{0.0};
    f64 finesKg{0.0};
    f64 coarseDebrisKg{0.0};

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] f64 TotalKg() const noexcept;
    [[nodiscard]] bool Empty(f64 epsilonKg = 0.0) const noexcept;

    SedimentMass& operator+=(const SedimentMass& other) noexcept;
    SedimentMass& operator-=(const SedimentMass& other) noexcept;
};

[[nodiscard]] SedimentMass operator+(
    SedimentMass left,
    const SedimentMass& right) noexcept;

[[nodiscard]] SedimentMass operator-(
    SedimentMass left,
    const SedimentMass& right) noexcept;

struct SedimentConversionRules
{
    // Existing loose material has a fixed class:
    // sand -> sand, soil/regolith -> fines, debris -> coarse.
    //
    // Only freshly eroded bedrock needs process-specific conversion.
    f64 hydraulicBedrockSandFraction{0.25};
    f64 aeolianBedrockSandFraction{0.85};

    // Glacier abrasion/plucking produces a mixed load. Coarse material models
    // plucked blocks/till; the remaining non-sand fraction is fines.
    f64 glacialBedrockSandFraction{0.25};
    f64 glacialBedrockCoarseFraction{0.50};

    // Surf abrasion is sand-rich but still creates suspended fines.
    f64 coastalBedrockSandFraction{0.60};

    // Thermal fracture always produces coarse debris.
    [[nodiscard]] bool IsValid() const noexcept;
};

struct MobileSedimentCell
{
    SedimentMass waterborne{};
    SedimentMass airborne{};
    SedimentMass surfaceMobile{};

    [[nodiscard]] SedimentMass& Medium(
        SedimentTransportMedium medium) noexcept;
    [[nodiscard]] const SedimentMass& Medium(
        SedimentTransportMedium medium) const noexcept;

    [[nodiscard]] SedimentMass Total() const noexcept;
};

enum class SedimentBoundarySide : u8
{
    North,
    East,
    South,
    West
};

struct SedimentTransportPacket
{
    SedimentMass waterborne{};
    SedimentMass airborne{};
    SedimentMass surfaceMobile{};

    [[nodiscard]] SedimentMass& Medium(
        SedimentTransportMedium medium) noexcept;
    [[nodiscard]] const SedimentMass& Medium(
        SedimentTransportMedium medium) const noexcept;

    [[nodiscard]] SedimentMass Total() const noexcept;
};

struct SedimentBoundaryFlux
{
    std::vector<SedimentTransportPacket> north;
    std::vector<SedimentTransportPacket> east;
    std::vector<SedimentTransportPacket> south;
    std::vector<SedimentTransportPacket> west;

    // NW, NE, SE, SW in the receiving page's local orientation.
    std::array<SedimentTransportPacket, 4> corners{};

    u64 revision{0};

    [[nodiscard]] bool IsComplete(u32 resolution) const noexcept;
    [[nodiscard]] SedimentMass Total() const noexcept;
};

struct SedimentDepositResult
{
    SedimentMass requested{};
    SedimentMass deposited{};
    SedimentMass remaining{};

    [[nodiscard]] f64 DepositedKg() const noexcept;
};

struct SedimentMassBalance
{
    SedimentMass physicalToMobile{};
    SedimentMass mobileToPhysical{};
    SedimentMass imported{};
    SedimentMass exported{};

    [[nodiscard]] SedimentMass NetBoundary() const noexcept;
};

// One M14 shared mobile-sediment page. M08 continues to own physical terrain.
// This object owns only sediment that is currently mobile between processes.
class SedimentExchangePage
{
public:
    SedimentExchangePage(
        u32 resolution,
        f64 spacingMeters,
        SedimentConversionRules conversionRules = {});

    [[nodiscard]] u32 Resolution() const noexcept;
    [[nodiscard]] f64 SpacingMeters() const noexcept;

    [[nodiscard]] MobileSedimentCell& At(u32 x, u32 y);
    [[nodiscard]] const MobileSedimentCell& At(
        u32 x,
        u32 y) const;

    [[nodiscard]] const SedimentConversionRules&
    ConversionRules() const noexcept;

    [[nodiscard]] const SedimentMassBalance&
    Accounting() const noexcept;

    void Add(
        u32 x,
        u32 y,
        SedimentTransportMedium medium,
        const SedimentMass& mass);

    // Publishes mass that has already been physically removed from M08 by a
    // process that needs the detailed MaterialRemoval for its own diagnostics.
    void PublishPhysicalRemoval(
        u32 x,
        u32 y,
        SedimentTransportMedium medium,
        const SedimentMass& mass);

    // Removes at most the requested amount, component by component.
    [[nodiscard]] SedimentMass Take(
        u32 x,
        u32 y,
        SedimentTransportMedium medium,
        const SedimentMass& requested);

    // Canonical M08 -> M14 conversion. The material is removed from the one
    // physical column and published into the selected shared mobile medium.
    [[nodiscard]] SedimentMass PickupFromColumn(
        terrain_material_column::MaterialColumnPage& material,
        const terrain_geology::GeologicalMaterialLibrary& geology,
        u32 x,
        u32 y,
        f64 depthMeters,
        SedimentSourceProcess sourceProcess,
        SedimentTransportMedium medium);

    // Canonical M14 -> M08 deposition. Deposition priority is coarse debris,
    // then sand, then fines. This mirrors the M08 exposed stack and prevents
    // finer material from consuming a constrained deposition budget before
    // coarse material settles.
    [[nodiscard]] SedimentDepositResult DepositToColumn(
        terrain_material_column::MaterialColumnPage& material,
        u32 x,
        u32 y,
        SedimentTransportMedium medium,
        f64 maximumTotalMassKg =
            std::numeric_limits<f64>::infinity());

    // Exports mobile mass across a physical page boundary. targetX/targetY are
    // neighbor coordinates in this page's local orientation and may be -1 or
    // resolution. Cross-cube-face remapping belongs to the M01 neighborhood
    // scheduler before the packet is imported by the receiving page.
    [[nodiscard]] SedimentMass ExportAcrossBoundary(
        u32 sourceX,
        u32 sourceY,
        i32 targetX,
        i32 targetY,
        SedimentTransportMedium medium,
        const SedimentMass& requested);

    // Returns and clears accumulated outgoing boundary flux.
    [[nodiscard]] SedimentBoundaryFlux TakeOutgoingBoundaryFlux(
        u64 revision);

    // Imports a complete boundary packet into the receiving page edge/corner
    // cells. The packet must already be remapped into this page's local
    // orientation.
    void ImportBoundaryFlux(
        const SedimentBoundaryFlux& incoming);

    [[nodiscard]] SedimentMass TotalMobileMass() const noexcept;

private:
    [[nodiscard]] std::size_t Index(u32 x, u32 y) const;

    void AccumulateBoundary(
        u32 sourceX,
        u32 sourceY,
        i32 targetX,
        i32 targetY,
        SedimentTransportMedium medium,
        const SedimentMass& mass);

    u32 resolution_{0};
    f64 spacingMeters_{0.0};
    SedimentConversionRules conversionRules_{};

    std::vector<MobileSedimentCell> cells_;
    SedimentBoundaryFlux outgoing_{};
    SedimentMassBalance accounting_{};
};

// Converts an M08 top-down removal into the M14 canonical classes.
[[nodiscard]] SedimentMass ClassifyRemovedMaterial(
    const terrain_material_column::MaterialRemoval& removal,
    const terrain_material_column::MaterialColumnPage& material,
    const terrain_geology::GeologicalMaterial& sourceRock,
    SedimentSourceProcess sourceProcess,
    const SedimentConversionRules& rules = {});

// GPU-ready shared representation: one float4 per medium.
// xyz = sand/fines/coarse kg per square meter; w reserved.
struct GpuSedimentMediumTexel
{
    f32 sandKgPerSquareMeter{0.0F};
    f32 finesKgPerSquareMeter{0.0F};
    f32 coarseDebrisKgPerSquareMeter{0.0F};
    f32 reserved{0.0F};
};

static_assert(sizeof(GpuSedimentMediumTexel) == 16);

struct GpuSedimentExchangePage
{
    u32 resolution{0};

    std::vector<GpuSedimentMediumTexel> waterborne;
    std::vector<GpuSedimentMediumTexel> airborne;
    std::vector<GpuSedimentMediumTexel> surfaceMobile;

    [[nodiscard]] std::size_t TexelCount() const noexcept;
    [[nodiscard]] std::size_t PackedByteSize() const noexcept;
};

[[nodiscard]] GpuSedimentExchangePage PackGpuSedimentExchangePage(
    const SedimentExchangePage& page);
} // namespace orbit::terrain_erosion
