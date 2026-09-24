#pragma once

#include <orbit/core/Types.hpp>

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
} // namespace orbit::studio_ui
