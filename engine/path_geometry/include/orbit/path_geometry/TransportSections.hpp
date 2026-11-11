#pragma once
#include <orbit/path_geometry/CorridorSweep.hpp>
#include <vector>

namespace orbit::path_geometry
{
struct RoadSectionDesign
{
    u32 laneCount{2};
    f64 laneWidthMeters{3.25};
    f64 shoulderMeters{0.75};
    f64 crownSlope{0.02};
    f64 shoulderSlope{0.03};
};
struct RailSectionDesign
{
    u32 trackCount{1};
    f64 gaugeMeters{1.435};
    f64 trackSpacingMeters{4.0};
    f64 railheadWidthMeters{0.07};
    f64 railheadHeightMeters{0.20};
    f64 formationMarginMeters{1.30};
    f64 sleeperPitchMeters{0.60};
};
struct RailContactReference
{
    f64 stationMeters{0};
    u32 trackIndex{0};
    math::Double3 leftRail{};
    math::Double3 rightRail{};
    math::Double3 tangent{};
    math::Double3 normal{};
};
struct RailSleeperPlacement
{
    f64 stationMeters{0};
    u32 trackIndex{0};
    math::Double3 centre{};
    math::Double3 tangent{};
    math::Double3 lateral{};
    math::Double3 up{};
};
// Pure derived recipes. Authored path/profile remains authoritative.
[[nodiscard]] CorridorSweep MakeRoadSweep(
    const RoadSectionDesign& design, std::vector<f64> bankRadians = {});
[[nodiscard]] CorridorSweep MakeRailSweep(
    const RailSectionDesign& design, std::vector<f64> cantRadians = {});
[[nodiscard]] std::vector<RailContactReference> RailContactReferences(
    const std::vector<PathStation>& stations,
    const RailSectionDesign& design, const std::vector<f64>& cantRadians = {});
[[nodiscard]] std::vector<RailSleeperPlacement> RailSleeperPlacements(
    const std::vector<PathStation>& stations,
    const RailSectionDesign& design,
    const std::vector<f64>& cantRadians = {},
    std::size_t maximumInstances = 200000U);
} // namespace orbit::path_geometry
