#pragma once

#include <orbit/content/ContentService.hpp>
#include <orbit/lighting/RuntimeMaterialEmission.hpp>
#include <orbit/studio_ui/LightingInteractionState.hpp>
#include <orbit/studio_session/StudioSession.hpp>
#include <orbit/world_model/MaterialAssignmentBinding.hpp>

#include <algorithm>
#include <optional>

namespace orbit::studio_ui
{
[[nodiscard]] inline StudioLightingInteractionDiagnostics
InspectSelectedLightingAuthority(
    const studio_session::StudioSession& session,
    const content::ContentService& content)
{
    StudioLightingInteractionDiagnostics result{};

    if (!session.World().HasWorld())
    {
        return result;
    }

    const auto& selection =
        session.World().Selection().Ordered();

    if (selection.size() != 1U)
    {
        return result;
    }

    const scene::ObjectId selected =
        selection.front();

    const auto record =
        session.World().Objects().Find(selected);

    if (!record.has_value())
    {
        return result;
    }

    result.hasSelection = true;
    result.selectedObject = record->name;

    const auto assignments =
        world_model::ResolveMaterialAssignments(
            session.World().Objects(),
            selected);

    const auto assignment =
        std::find_if(
            assignments.begin(),
            assignments.end(),
            [selected](
                const world_model::ResolvedMaterialAssignment& item)
            {
                return
                    item.owner == selected &&
                    item.slot == "default";
            });

    if (assignment == assignments.end())
    {
        return result;
    }

    try
    {
        const auto emission =
            lighting::ResolveRuntimeMaterialEmission(
                content,
                assignment->assetId);

        result.emissionLuminanceNits =
            std::max<f64>(
                static_cast<f64>(
                    emission.physical.luminanceNits),
                0.0);
        result.emissionGiScale =
            std::max<f64>(
                static_cast<f64>(
                    emission.physical.giScale),
                0.0);
        result.emissionGiEnabled =
            emission.physical.contributesToGi &&
            result.emissionGiScale > 0.0;
        result.selectedEmissive =
            result.emissionLuminanceNits > 0.0;
    }
    catch (...)
    {
        // Selection inspection is diagnostic-only. A missing/stale material
        // must not destabilize the active renderer or authoring session.
        result.selectedEmissive = false;
        result.emissionLuminanceNits = 0.0;
        result.emissionGiScale = 0.0;
        result.emissionGiEnabled = false;
    }

    return result;
}

inline void PublishSelectedLightingAuthority(
    const studio_session::StudioSession& session,
    const content::ContentService& content)
{
    auto selected =
        InspectSelectedLightingAuthority(
            session,
            content);

    auto& live =
        StudioLightingInteractionState();

    // Selection ownership is updated independently from the per-frame cache/
    // visibility diagnostics populated by the viewport renderer.
    live.hasSelection = selected.hasSelection;
    live.selectedObject =
        std::move(selected.selectedObject);
    live.selectedEmissive =
        selected.selectedEmissive;
    live.emissionLuminanceNits =
        selected.emissionLuminanceNits;
    live.emissionGiScale =
        selected.emissionGiScale;
    live.emissionGiEnabled =
        selected.emissionGiEnabled;
}
} // namespace orbit::studio_ui
