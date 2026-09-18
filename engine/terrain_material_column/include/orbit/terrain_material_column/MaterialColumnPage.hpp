#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_impacts/ImpactField.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace orbit::terrain_material_column
{
enum class LooseMaterialKind : u8
{
    Regolith,
    Soil,
    Sand,
    Debris
};

enum class ExposedSurfaceKind : u8
{
    Bedrock,
    Regolith,
    Soil,
    Sand,
    Debris
};

struct LooseMaterialDensities
{
    f32 regolithKgPerCubicMeter{1'650.0F};
    f32 soilKgPerCubicMeter{1'300.0F};
    f32 sandKgPerCubicMeter{1'600.0F};
    f32 debrisKgPerCubicMeter{1'850.0F};

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] f32 Density(
        LooseMaterialKind kind) const noexcept;
};

struct MaterialColumnCell
{
    // Absolute radial/elevation coordinate of the bedrock surface.
    f32 bedrockHeightMeters{0.0F};

    // Initial/reference bedrock surface for queryable bedrock-removal mass.
    // M08 mutations lower bedrockHeightMeters; later uplift/rebase systems may
    // explicitly reset this reference when establishing a new process epoch.
    f32 referenceBedrockHeightMeters{0.0F};

    terrain_geology::RockTypeId bedrockMaterial{};

    // Fixed semantic stack, bottom -> top:
    // bedrock, regolith, soil, sand, debris.
    f32 regolithMeters{0.0F};
    f32 soilMeters{0.0F};
    f32 sandMeters{0.0F};
    f32 debrisMeters{0.0F};

    // Normalized pore/surface moisture. Exact hydrology ownership arrives in
    // later milestones; this is the physical material-column storage channel.
    f32 moisture{0.0F};

    // Scratch/process channel mirrored to the GPU page. Never project authority.
    f32 temporaryScalar{0.0F};

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] f32 LooseDepthMeters() const noexcept;
    [[nodiscard]] f32 SurfaceHeightMeters() const noexcept;
    [[nodiscard]] ExposedSurfaceKind ExposedSurface() const noexcept;
};

struct MaterialRemoval
{
    f64 requestedDepthMeters{0.0};
    f64 removedDepthMeters{0.0};

    f64 debrisMeters{0.0};
    f64 sandMeters{0.0};
    f64 soilMeters{0.0};
    f64 regolithMeters{0.0};
    f64 bedrockMeters{0.0};

    f64 removedMassKg{0.0};
};

struct MaterialMassSummary
{
    f64 regolithKg{0.0};
    f64 soilKg{0.0};
    f64 sandKg{0.0};
    f64 debrisKg{0.0};

    // Bedrock removed below each cell's reference surface.
    f64 excavatedBedrockKg{0.0};

    [[nodiscard]] f64 LooseMassKg() const noexcept;
    [[nodiscard]] f64 AccountedMassKg() const noexcept;
};

class MaterialColumnPage
{
public:
    MaterialColumnPage(
        u32 resolution,
        f64 spacingMeters,
        LooseMaterialDensities densities = {});

    [[nodiscard]] u32 Resolution() const noexcept;
    [[nodiscard]] f64 SpacingMeters() const noexcept;
    [[nodiscard]] f64 CellAreaSquareMeters() const noexcept;

    [[nodiscard]] MaterialColumnCell& At(u32 x, u32 y);
    [[nodiscard]] const MaterialColumnCell& At(
        u32 x, u32 y) const;

    // Replaces a cell and validates it. When establishReference=true, the
    // supplied current bedrock height becomes the process mass reference.
    void SetCell(
        u32 x,
        u32 y,
        MaterialColumnCell cell,
        bool establishReference = true);

    // Moves the geological substrate without creating/removing material.
    // Used by tectonic/uplift processes. When shiftReference=true the bedrock
    // excavation mass reference moves with the substrate, so uplift/subsidence
    // is not misreported as erosion.
    void DisplaceBedrock(
        u32 x,
        u32 y,
        f64 deltaMeters,
        bool shiftReference = true);

    [[nodiscard]] MaterialRemoval Erode(
        u32 x,
        u32 y,
        f64 depthMeters,
        const terrain_geology::GeologicalMaterialLibrary& geology);

    // Deposits loose material without changing geological substrate identity.
    // Any positive deposit therefore covers exposed bedrock naturally.
    [[nodiscard]] f64 Deposit(
        u32 x,
        u32 y,
        LooseMaterialKind kind,
        f64 depthMeters);

    // M07 -> M08 coupling: excavation removes the fixed top-down stack and
    // ejecta becomes debris. Ray intensity is retained in the scratch channel.
    [[nodiscard]] MaterialRemoval ApplyImpact(
        u32 x,
        u32 y,
        const terrain_impacts::CraterProcessSample& impact,
        const terrain_geology::GeologicalMaterialLibrary& geology);

    [[nodiscard]] MaterialMassSummary QueryMass(
        const terrain_geology::GeologicalMaterialLibrary& geology) const;

    [[nodiscard]] std::span<const MaterialColumnCell>
    Cells() const noexcept;

    [[nodiscard]] const LooseMaterialDensities&
    Densities() const noexcept;

private:
    [[nodiscard]] std::size_t Index(u32 x, u32 y) const;

    u32 resolution_{0};
    f64 spacingMeters_{0.0};
    LooseMaterialDensities densities_{};
    std::vector<MaterialColumnCell> cells_;
};

// Exact GPU texture-lane mirrors for the target M08 packing:
//
// R32F      bedrock height
// RGBA16F   regolith / soil / sand / debris
// RG16F     moisture / temporary scalar
// R16_UINT  geological material table index
struct GpuLooseMaterialTexel
{
    u16 regolith{0};
    u16 soil{0};
    u16 sand{0};
    u16 debris{0};
};

struct GpuMoistureProcessTexel
{
    u16 moisture{0};
    u16 temporaryScalar{0};
};

static_assert(sizeof(GpuLooseMaterialTexel) == 8);
static_assert(sizeof(GpuMoistureProcessTexel) == 4);

struct GpuMaterialColumnPage
{
    u32 resolution{0};

    std::vector<f32> bedrockHeightR32F;
    std::vector<GpuLooseMaterialTexel> looseRgba16F;
    std::vector<GpuMoistureProcessTexel> moistureProcessRg16F;
    std::vector<u16> geologicalMaterialR16Uint;

    [[nodiscard]] std::size_t TexelCount() const noexcept;
    [[nodiscard]] std::size_t PackedByteSize() const noexcept;
    [[nodiscard]] static constexpr std::size_t
    PackedBytesPerTexel() noexcept
    {
        return sizeof(f32) +
            sizeof(GpuLooseMaterialTexel) +
            sizeof(GpuMoistureProcessTexel) +
            sizeof(u16);
    }
};

static_assert(
    GpuMaterialColumnPage::PackedBytesPerTexel() == 18,
    "M08 target packing is 18 bytes/texel across four GPU image lanes.");

[[nodiscard]] GpuMaterialColumnPage PackGpuPage(
    const MaterialColumnPage& page,
    const terrain_geology::GeologicalMaterialGpuTable& geologyTable);

// Exposed for deterministic codec tests and CPU debug readback only.
[[nodiscard]] u16 FloatToHalfBits(f32 value) noexcept;
[[nodiscard]] f32 HalfBitsToFloat(u16 bits) noexcept;

} // namespace orbit::terrain_material_column
