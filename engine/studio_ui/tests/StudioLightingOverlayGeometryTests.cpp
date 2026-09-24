#include <orbit/studio_ui/StudioLightingOverlayGeometry.hpp>

#include <cstdlib>

int main()
{
    using namespace orbit;

    lighting::LightingView view{};
    view.frame = frames::FrameId{1U, 2U};
    view.body = universe::BodyId{3U, 4U};
    view.cameraInFrameMeters = {0.0, 0.0, 0.0};
    view.gpuOriginInFrameMeters = {0.0, 0.0, 0.0};

    const lighting::RadianceClipmapConfig config{
        .baseCellSizeMeters = 2.0,
        .levelScale = 4.0,
        .levelCount = 4U,
        .cellsPerAxis = 8U
    };

    const lighting::RadianceCellKey key{
        .frame = view.frame,
        .body = view.body,
        .level = 0U,
        .x = 1,
        .y = 2,
        .z = 3
    };

    const lighting::RadianceUpdateCandidate candidate{
        .key = key,
        .physicalIndex = 0U,
        .priority = 2.0F,
        .ageSeconds = 1.0F
    };

    const auto gi = studio_ui::BuildGiUpdateCellOverlayLines(
        std::span<const lighting::RadianceUpdateCandidate>(&candidate, 1U),
        config,
        view,
        1U);

    if (gi.size() != 12U)
    {
        return 1;
    }

    const auto regions = studio_ui::BuildRadianceCacheRegionOverlayLines(
        {0.0, 0.0, 0.0},
        config,
        view,
        3U);

    if (regions.size() != 36U)
    {
        return 2;
    }

    const lighting::DynamicEmissiveSourceState emissive{
        .stableId = 10U,
        .contentRevision = 11U,
        .centerInFrameMeters = {2.0, 3.0, 4.0},
        .sourceRadiusMeters = 1.0,
        .influenceRangeMeters = 5.0
    };

    const auto influence = studio_ui::BuildEmissiveInfluenceOverlayLines(
        std::span<const lighting::DynamicEmissiveSourceState>(&emissive, 1U),
        view,
        1U);

    if (influence.size() != 96U)
    {
        return 3;
    }

    const auto capped = studio_ui::BuildGiUpdateCellOverlayLines(
        std::span<const lighting::RadianceUpdateCandidate>(&candidate, 1U),
        config,
        view,
        0U);

    if (capped.size() != 12U)
    {
        return 4;
    }

    return EXIT_SUCCESS;
}
