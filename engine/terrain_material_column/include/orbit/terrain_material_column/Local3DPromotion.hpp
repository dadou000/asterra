#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace orbit::terrain_material_column
{
// M24 is a sparse promotion of the normal M08 heightfield/material-column
// representation. Ordinary terrain owns no Local3DPromotionRegion object.
enum class Local3DPromotionReason : u8
{
    ExplicitAuthoring,
    CaveMouth,
    UndercutCliff,
    Arch,
    ProcessUndercut
};

struct Local3DPromotionPolicy
{
    bool allowProcessTriggered{true};

    f64 minimumVoidHeightMeters{1.0};
    f64 minimumUndercutMeters{0.5};
    f64 minimumPersistenceMeters{3.0};
    f32 minimumProcessConfidence{0.85F};

    // This ring remains the original single-span heightfield geometry. Local
    // 3D edits are rejected there, guaranteeing a stable promoted boundary.
    u32 boundaryGuardCells{2};

    // Finite local vertical window; M24 is not a planet-wide voxel volume.
    f64 belowSurfaceWindowMeters{64.0};
    f64 aboveSurfaceWindowMeters{32.0};

    u32 maximumSolidSpansPerCell{6};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct Local3DPromotionRequest
{
    terrain::PhysicalTerrainPageAddress address{};

    Local3DPromotionReason reason{
        Local3DPromotionReason::ExplicitAuthoring};

    // Revisions belong to physical/promoted state, not view residency.
    u64 sourceRevision{0};
    u64 promotionRevision{0};

    // Evidence for process-triggered promotion. Explicit authoring does not
    // need to satisfy these thresholds, but values must still be finite.
    f64 voidHeightMeters{0.0};
    f64 undercutMeters{0.0};
    f64 persistenceMeters{0.0};
    f32 processConfidence{0.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

[[nodiscard]] bool ShouldPromoteLocal3D(
    const Local3DPromotionRequest& request,
    const Local3DPromotionPolicy& policy) noexcept;

[[nodiscard]] u64 Local3DPromotionFingerprint(
    const Local3DPromotionRequest& request,
    const Local3DPromotionPolicy& policy) noexcept;

struct Local3DSolidSpan
{
    f32 bottomHeightMeters{0.0F};
    f32 topHeightMeters{0.0F};
    terrain_geology::RockTypeId material{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct Local3DCell
{
    // Ordered bottom -> top. Gaps between spans are true voids and can
    // therefore represent cave space, arches and undercut roofs.
    std::vector<Local3DSolidSpan> solidSpans;

    [[nodiscard]] bool IsValid(
        u32 maximumSolidSpans) const noexcept;

    [[nodiscard]] bool ContainsSolid(
        f32 heightMeters) const noexcept;

    [[nodiscard]] std::optional<f32>
    TopSurfaceHeightMeters() const noexcept;
};

class Local3DPromotionRegion
{
public:
    Local3DPromotionRegion(
        const MaterialColumnPage& source,
        Local3DPromotionRequest request,
        Local3DPromotionPolicy policy = {});

    [[nodiscard]] u32 Resolution() const noexcept;
    [[nodiscard]] f64 SpacingMeters() const noexcept;

    [[nodiscard]] const Local3DPromotionRequest&
    Request() const noexcept;

    [[nodiscard]] const Local3DPromotionPolicy&
    Policy() const noexcept;

    [[nodiscard]] u64 Fingerprint() const noexcept;

    [[nodiscard]] bool IsBoundaryGuardCell(
        u32 x,
        u32 y) const;

    [[nodiscard]] Local3DCell& At(u32 x, u32 y);
    [[nodiscard]] const Local3DCell& At(
        u32 x,
        u32 y) const;

    // Removes solid material in a vertical interval. Interior subtraction may
    // split one span into two, producing a genuine void below an overhang.
    [[nodiscard]] bool CarveVoid(
        u32 x,
        u32 y,
        f32 bottomHeightMeters,
        f32 topHeightMeters);

    // Adds a disconnected/adjacent structural rock interval. Overlap with a
    // different material is rejected instead of silently changing authority.
    [[nodiscard]] bool AddSolid(
        u32 x,
        u32 y,
        Local3DSolidSpan span);

    [[nodiscard]] bool BoundaryMatches(
        const MaterialColumnPage& source,
        f64 toleranceMeters = 1.0e-4) const;

    [[nodiscard]] std::size_t ApproximateOwnedBytes() const noexcept;

private:
    [[nodiscard]] std::size_t Index(u32 x, u32 y) const;

    [[nodiscard]] bool IntervalInsideWindow(
        std::size_t index,
        f32 bottomHeightMeters,
        f32 topHeightMeters) const noexcept;

    u32 resolution_{0};
    f64 spacingMeters_{0.0};

    Local3DPromotionRequest request_{};
    Local3DPromotionPolicy policy_{};
    u64 fingerprint_{0};

    std::vector<f32> baselineSurfaceHeights_;
    std::vector<Local3DCell> cells_;
};

// Returns null for normal terrain or a process request that fails the strict
// promotion policy. This is the no-persistent-cost boundary for ordinary land.
[[nodiscard]] std::unique_ptr<Local3DPromotionRegion>
TryPromoteLocal3D(
    const MaterialColumnPage& source,
    const Local3DPromotionRequest& request,
    const Local3DPromotionPolicy& policy = {});
} // namespace orbit::terrain_material_column
