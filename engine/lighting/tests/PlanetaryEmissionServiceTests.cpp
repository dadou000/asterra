#include <orbit/lighting/PlanetaryEmissionService.hpp>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    const frames::FrameId frame{
        .high = 1U,
        .low = 2U};
    const universe::BodyId body{
        .high = 3U,
        .low = 4U};

    RuntimeEmissiveSurface surface{
        .geometry = {
            .frame = frame,
            .body = body,
            .stableId = 100U,
            .contentRevision = 1U,
            .originInFrameMeters =
                {10.0, -1.0, -1.0},
            .axisUInFrameMeters =
                {0.0, 2.0, 0.0},
            .axisVInFrameMeters =
                {0.0, 0.0, 2.0}
        },
        .contentRevision = 1U,
        .width = 2U,
        .height = 2U,
        .giRadiance = {
            {5.0F, 1.0F, 0.0F},
            {5.0F, 1.0F, 0.0F},
            {5.0F, 1.0F, 0.0F},
            {5.0F, 1.0F, 0.0F}
        }
    };

    PlanetaryEmissionService service({
        .baseWidth = 8U,
        .baseHeight = 4U,
        .maximumLevels = 4U
    });

    if (!service.Upsert(surface))
    {
        return 1;
    }

    if (service.Upsert(surface))
    {
        return 2;
    }

    const auto* first =
        service.Resolve(
            frame,
            body,
            {});

    if (first == nullptr ||
        first->levels.empty() ||
        service.Stats().rebuildCount != 1U ||
        service.Stats().sourceCount != 1U)
    {
        return 3;
    }

    const auto* reused =
        service.Resolve(
            frame,
            body,
            {});

    if (reused != first ||
        service.Stats().rebuildCount != 1U)
    {
        return 4;
    }

    surface.contentRevision = 2U;
    surface.geometry.contentRevision = 2U;
    surface.giRadiance[0] =
        {20.0F, 2.0F, 0.0F};

    if (!service.Upsert(surface))
    {
        return 5;
    }

    const auto* changed =
        service.Resolve(
            frame,
            body,
            {});

    if (changed == nullptr ||
        service.Stats().rebuildCount != 2U)
    {
        return 6;
    }

    const auto total =
        TotalPlanetaryIntegratedRadianceArea(
            changed->levels.front());

    if (total.x <= 0.0F)
    {
        return 7;
    }

    if (!service.Remove(
            body,
            100U) ||
        service.Resolve(
            frame,
            body,
            {}) != nullptr)
    {
        return 8;
    }

    return 0;
}
