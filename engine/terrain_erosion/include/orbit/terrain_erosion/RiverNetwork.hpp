#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_erosion/SedimentExchange.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <optional>
#include <span>
#include <vector>

namespace orbit::terrain_erosion
{
struct RiverNodeIdTag;
using RiverNodeId = core::StrongId<RiverNodeIdTag>;

struct RiverSegmentIdTag;
using RiverSegmentId = core::StrongId<RiverSegmentIdTag>;

struct RiverBasinIdTag;
using RiverBasinId = core::StrongId<RiverBasinIdTag>;

struct RiverConstraintIdTag;
using RiverConstraintId = core::StrongId<RiverConstraintIdTag>;

enum class RiverConstraintKind : u8
{
    Attract,
    Repel,
    Trajectory
};

// Basin-local authored river intent. Coordinates are in the physical page's
// local metre frame: x=east, y=south. A trajectory is the infinite local line
// through centerMeters in normalized directionMeters.
struct RiverConstraint
{
    RiverConstraintId id{};
    RiverBasinId targetBasin{};
    RiverConstraintKind kind{RiverConstraintKind::Attract};

    math::Double2 centerMeters{};
    math::Double2 directionMeters{1.0, 0.0};

    f64 radiusMeters{100.0};
    f64 strength{1.0};
    u64 revision{0};
    bool enabled{true};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct RiverNetworkConfig
{
    f64 minimumDrainageAreaSquareMeters{25'000.0};
    f64 minimumDischargeCubicMetersPerSecond{0.05};

    f64 referenceDischargeCubicMetersPerSecond{10.0};

    f64 baseChannelWidthMeters{4.0};
    f64 minimumChannelWidthMeters{1.0};
    f64 maximumChannelWidthMeters{180.0};
    f64 widthDischargeExponent{0.45};

    f64 baseChannelDepthMeters{1.5};
    f64 minimumChannelDepthMeters{0.25};
    f64 maximumChannelDepthMeters{35.0};
    f64 depthDischargeExponent{0.30};

    // Optional centreline evolution. The drainage graph remains the
    // topological source of truth; meandering changes physical channel
    // position inside a bounded corridor.
    bool enableMeanders{true};
    u32 meanderIterations{8U};
    f64 meanderTimeStep{1.0};
    f64 curvatureMigrationRate{0.35};
    f64 deterministicSeedMigrationRate{0.10};
    f64 maximumCenterlineOffsetWidths{3.0};

    // Optional neck cutoff. A candidate must be separated by at least this
    // many directed nodes along the existing path.
    bool enableCutoffs{true};
    u32 minimumCutoffPathNodes{4U};
    f64 cutoffDistanceWidths{1.5};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct RiverNetworkNode
{
    RiverNodeId id{};
    RiverBasinId basin{};

    u32 sourceX{0};
    u32 sourceY{0};

    math::Double2 drainageOffsetMeters{};
    math::Double2 channelOffsetMeters{};

    f32 surfaceHeightMeters{0.0F};
    f32 drainageElevationMeters{0.0F};

    f64 drainageAreaSquareMeters{0.0};
    f64 dischargeCubicMetersPerSecond{0.0};

    f32 channelWidthMeters{0.0F};
    f32 channelDepthMeters{0.0F};

    i8 drainageFlowDx{0};
    i8 drainageFlowDy{0};
    bool exitsPage{false};
    bool outlet{false};
};

struct RiverNetworkSegment
{
    RiverSegmentId id{};
    RiverBasinId basin{};

    u32 upstreamNode{0};
    u32 downstreamNode{0};

    bool active{true};
    bool cutoffSegment{false};
};

struct RiverBoundaryLink
{
    RiverNodeId upstreamNode{};
    RiverBasinId basin{};

    i8 flowDx{0};
    i8 flowDy{0};

    // Local coordinate just outside the page reached by the M09 flow.
    i32 targetX{0};
    i32 targetY{0};
};

struct RiverCutoffEvent
{
    RiverNodeId upstreamNode{};
    RiverNodeId downstreamNode{};
    RiverSegmentId newSegment{};

    std::vector<RiverSegmentId> oxbowSegments;
};

struct RiverBasinState
{
    RiverBasinId id{};

    u64 drainageRevision{0};
    u64 constraintRevision{0};
    u64 revision{0};

    u32 nodeCount{0};
    u32 activeSegmentCount{0};
};

struct RiverNetwork
{
    terrain::PhysicalTerrainPageKey sourcePage{};
    u64 drainageRevision{0};

    u32 resolution{0};
    f64 spacingMeters{0.0};

    std::vector<RiverNetworkNode> nodes;
    std::vector<RiverNetworkSegment> segments;
    std::vector<RiverBoundaryLink> boundaryLinks;
    std::vector<RiverCutoffEvent> cutoffEvents;
    std::vector<RiverBasinState> basins;

    [[nodiscard]] const RiverNetworkNode* FindNode(
        RiverNodeId id) const noexcept;

    [[nodiscard]] const RiverNetworkSegment* FindSegment(
        RiverSegmentId id) const noexcept;

    [[nodiscard]] const RiverBasinState* FindBasin(
        RiverBasinId id) const noexcept;
};

struct RiverSelection
{
    RiverNodeId node{};
    RiverSegmentId segment{};
    RiverBasinId basin{};

    f64 distanceMeters{0.0};

    [[nodiscard]] bool HasNode() const noexcept
    {
        return node.IsValid();
    }

    [[nodiscard]] bool HasSegment() const noexcept
    {
        return segment.IsValid();
    }
};

// Deterministic M16 graph extraction from M09 drainage. River node/segment and
// basin IDs derive from stable physical page address + physical source cells;
// terrain revisions therefore rebuild derived state without changing Studio
// selection identity.
[[nodiscard]] RiverNetwork BuildRiverNetwork(
    const terrain_hydrology::DrainagePage& drainage,
    std::span<const RiverConstraint> constraints = {},
    const RiverNetworkConfig& config = {});

// Selectable Studio-facing query over stable node/segment IDs.
[[nodiscard]] RiverSelection SelectNearestRiver(
    const RiverNetwork& network,
    const math::Double2& localMeters,
    f64 maximumDistanceMeters);

// Returns only the authored revision affecting this basin. Editing another
// basin cannot invalidate this basin's river process revision.
[[nodiscard]] u64 RiverBasinConstraintRevision(
    RiverBasinId basin,
    std::span<const RiverConstraint> constraints) noexcept;

struct RiverIncisionConfig
{
    // Channel bed is lowered by the node/segment discharge-derived depth.
    // Valley shoulders fade to zero erosion at this width multiplier.
    f64 valleyWidthMultiplier{3.0};
    f64 maximumIncisionMetersPerBake{10.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct RiverIncisionResult
{
    f64 erodedMassKg{0.0};
    f64 erodedDepthMeters{0.0};
    u32 affectedCells{0};

    // Shared M14 mobile load after physical incision.
    f64 finalWaterborneSedimentKg{0.0};
};

// Bakes the evolved M16 centreline into the one M08 physical material column.
// Removed material is published through M14 waterborne sediment, so river
// position and physical incision cannot diverge into separate authorities.
[[nodiscard]] RiverIncisionResult ApplyRiverNetworkIncision(
    terrain_material_column::MaterialColumnPage& material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    SedimentExchangePage& sediment,
    const RiverNetwork& network,
    const RiverIncisionConfig& config = {});
} // namespace orbit::terrain_erosion
