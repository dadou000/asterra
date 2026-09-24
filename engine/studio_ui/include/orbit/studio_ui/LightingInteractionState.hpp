#pragma once

#include <orbit/content/ContentService.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/world_model/MaterialAssignmentBinding.hpp>

#include <algorithm>
#include <string>

namespace orbit::studio_ui
{
struct StudioLightingOverlayOptions
{
    bool giUpdateCells{false};
    bool radianceCacheRegions{false};
    bool reflectionInspection{false};
    bool emissiveInfluence{false};
    u32 maximumGiCells{48U};
    u32 cacheLevels{3U};
};

struct StudioLightingInteractionDiagnostics
{
    bool hasSelection{false};
    std::string selectedObject;
    bool selectedEmissive{false};
    f64 emissionLuminanceNits{0.0};
    f64 emissionGiScale{0.0};
    bool emissionGiEnabled{false};

    u32 trackedEmissiveSources{0U};
    u32 invalidationEventsThisFrame{0U};
    u64 dirtyRadianceCells{0U};
    u32 scheduledRadianceUpdates{0U};

    bool hardwareRayQuerySupported{false};
    bool hardwareRayQueryReady{false};
    u32 hardwarePrimitiveCount{0U};
};

[[nodiscard]] inline StudioLightingOverlayOptions&
StudioLightingOverlays() noexcept
{
    static StudioLightingOverlayOptions state{};
    return state;
}

[[nodiscard]] inline StudioLightingInteractionDiagnostics&
StudioLightingInteractionState() noexcept
{
    static StudioLightingInteractionDiagnostics state{};
    return state;
}

inline void InspectSelectedEmissiveGi(
    StudioLightingInteractionDiagnostics& diagnostics,
    studio_session::StudioSession& session,
    content::ContentService& content)
{
    diagnostics.hasSelection = false;
    diagnostics.selectedObject.clear();
    diagnostics.selectedEmissive = false;
    diagnostics.emissionLuminanceNits = 0.0;
    diagnostics.emissionGiScale = 0.0;
    diagnostics.emissionGiEnabled = false;

    if (!session.World().HasWorld() ||
        session.World().Selection().Ordered().size() != 1U)
    {
        return;
    }

    const auto selected =
        session.World().Selection().Ordered().front();
    diagnostics.hasSelection = true;
    diagnostics.selectedObject = selected.ToString();

    for (const auto& assignment :
         world_model::ResolveMaterialAssignments(
             session.World().Objects(),
             selected))
    {
        if (assignment.owner != selected)
        {
            continue;
        }

        const content::AssetRecord* asset =
            content.FindByPath(assignment.assetId);

        if (asset == nullptr)
        {
            if (const auto id =
                    content::AssetId::Parse(
                        assignment.assetId);
                id.has_value())
            {
                asset = content.Find(*id);
            }
        }

        if (asset == nullptr)
        {
            continue;
        }

        const auto emission =
            content.ResolveMaterialEmission(asset->id);

        diagnostics.emissionLuminanceNits =
            std::max(
                diagnostics.emissionLuminanceNits,
                emission.luminanceNits);
        diagnostics.emissionGiScale =
            std::max(
                diagnostics.emissionGiScale,
                emission.giScale);
        diagnostics.emissionGiEnabled =
            diagnostics.emissionGiEnabled ||
            emission.contributesToGi;
        diagnostics.selectedEmissive =
            diagnostics.selectedEmissive ||
            emission.luminanceNits > 0.0;
    }
}
} // namespace orbit::studio_ui
