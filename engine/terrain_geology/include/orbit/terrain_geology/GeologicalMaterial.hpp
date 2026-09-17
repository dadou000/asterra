#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orbit::terrain_geology
{
struct RockTypeIdTag;
using RockTypeId = core::StrongId<RockTypeIdTag>;

// Authored geological substrate record. All normalized coefficients are in
// [0, 1]. Density is bulk density in kg/m^3. This record is CPU authority;
// shaders consume only the compact GpuGeologicalMaterial representation.
struct GeologicalMaterial
{
    RockTypeId id{};
    std::string name;

    f32 hardness{0.5F};
    f32 cohesion{0.5F};
    f32 hydraulicErodibility{0.5F};
    f32 aeolianErodibility{0.5F};
    f32 permeability{0.5F};
    f32 chemicalWeatherability{0.5F};
    f32 fractureTendency{0.5F};
    f32 density{2'500.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

// Fixed shader-facing layout: two float4 lanes, 32 bytes per rock type.
// Rock identity stays in the CPU-side lookup table; GPU terrain pages store a
// compact table index rather than repeating 128-bit authored IDs per texel.
struct alignas(16) GpuGeologicalMaterial
{
    f32 hardness{0.0F};
    f32 cohesion{0.0F};
    f32 hydraulicErodibility{0.0F};
    f32 aeolianErodibility{0.0F};

    f32 permeability{0.0F};
    f32 chemicalWeatherability{0.0F};
    f32 fractureTendency{0.0F};
    f32 density{0.0F};
};

static_assert(
    sizeof(GpuGeologicalMaterial) == 32,
    "M02 GPU geological materials must remain two float4 lanes.");
static_assert(
    alignof(GpuGeologicalMaterial) == 16,
    "M02 GPU geological materials must remain float4 aligned.");

[[nodiscard]] GpuGeologicalMaterial ToGpuMaterial(
    const GeologicalMaterial& material) noexcept;

struct GeologicalMaterialGpuTable
{
    std::vector<RockTypeId> rockTypes;
    std::vector<GpuGeologicalMaterial> materials;

    [[nodiscard]] std::optional<u32> IndexOf(
        RockTypeId id) const noexcept;
};

// CPU-authoritative material library. Mutation increments a monotonic revision
// so downstream derived GPU tables/pages can invalidate without becoming
// authoring authorities themselves.
class GeologicalMaterialLibrary
{
public:
    void Upsert(GeologicalMaterial material);
    [[nodiscard]] bool Erase(RockTypeId id);

    [[nodiscard]] const GeologicalMaterial* Find(
        RockTypeId id) const noexcept;
    [[nodiscard]] const GeologicalMaterial* FindByName(
        std::string_view name) const noexcept;

    [[nodiscard]] std::span<const GeologicalMaterial>
    Materials() const noexcept;

    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] u64 Revision() const noexcept;

    [[nodiscard]] GeologicalMaterialGpuTable
    BuildGpuTable() const;

private:
    void RebuildIndex();

    std::vector<GeologicalMaterial> materials_;
    std::unordered_map<RockTypeId, std::size_t> index_;
    u64 revision_{0};
};

// Common process forcing used only to prove/consume intrinsic differential
// response. Later process milestones can resolve climate/biome/authoring
// modifiers on top; these values remain geology-owned intrinsic coefficients.
struct GeologicalErosionForcing
{
    f32 hydraulic{0.0F};
    f32 aeolian{0.0F};
    f32 chemical{0.0F};
    f32 fracture{0.0F};
};

struct GeologicalErosionResponse
{
    f32 hydraulicDetachment{0.0F};
    f32 aeolianDetachment{0.0F};
    f32 chemicalWeathering{0.0F};
    f32 fracturePotential{0.0F};

    [[nodiscard]] f32 Total() const noexcept;
};

[[nodiscard]] GeologicalErosionResponse EvaluateIntrinsicErosionResponse(
    const GeologicalMaterial& material,
    const GeologicalErosionForcing& forcing) noexcept;

// Stable IDs for the initial authored reference records. These are persisted
// identity values, not array indices; append new reference materials rather
// than renumbering/reusing them.
namespace reference_rock
{
inline constexpr RockTypeId Basalt{
    .high = 0x4F52424954524F43ULL,
    .low = 0x4B00000000000001ULL
};
inline constexpr RockTypeId Granite{
    .high = 0x4F52424954524F43ULL,
    .low = 0x4B00000000000002ULL
};
inline constexpr RockTypeId Sandstone{
    .high = 0x4F52424954524F43ULL,
    .low = 0x4B00000000000003ULL
};
inline constexpr RockTypeId Limestone{
    .high = 0x4F52424954524F43ULL,
    .low = 0x4B00000000000004ULL
};
inline constexpr RockTypeId VolcanicAsh{
    .high = 0x4F52424954524F43ULL,
    .low = 0x4B00000000000005ULL
};
} // namespace reference_rock

[[nodiscard]] std::array<GeologicalMaterial, 5>
EarthReferenceGeologicalMaterials();

void RegisterEarthReferenceGeologicalMaterials(
    GeologicalMaterialLibrary& library);

// Project-authority codec for .orbitgeologicalmaterial files. This is the
// physical-data boundary that content/editor indexing can discover without
// moving geological semantics into the renderer material system.
[[nodiscard]] GeologicalMaterial ParseGeologicalMaterialToml(
    std::string_view text);

[[nodiscard]] std::string SerializeGeologicalMaterialToml(
    const GeologicalMaterial& material);
} // namespace orbit::terrain_geology
