#pragma once
#include <orbit/path_geometry/JunctionConflicts.hpp>
#include <vector>

namespace orbit::path_geometry
{
struct JunctionPhase
{
    // Index into the input JunctionPlan::movements, NOT a stable author ID.
    std::vector<u32> movementIndices;
};
struct JunctionPhasePlan
{
    std::vector<JunctionPhase> phases;
    bool valid{false};
    bool requiresRailInterlocking{false};
    // Advisory compatibility partition ONLY: no phase timings or priority.
};
// Deterministic greedy conflict-graph colouring, with hard size limits.
// Refuses incomplete conflict analysis and mixed rail movements. No runtime
// signal timing, jurisdictional standards or junction reservation implied.
[[nodiscard]] JunctionPhasePlan SuggestRoadJunctionPhases(
    const JunctionPlan& plan, const JunctionConflictReport& conflicts,
    u32 maximumPhases = 32U);
} // namespace orbit::path_geometry
