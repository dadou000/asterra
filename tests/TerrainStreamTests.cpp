#include <orbit/terrain_stream/ToroidalResidency.hpp>

#include <iostream>

namespace
{
orbit::terrain_view::ClipmapMotionUpdate MakeMotion(
    const orbit::i64 shiftX,
    const orbit::i64 shiftY,
    const bool fullRefresh)
{
    orbit::terrain_view::ClipmapMotionUpdate update{};
    update.levels.push_back({
        .levelIndex = 0,
        .cellShiftX = shiftX,
        .cellShiftY = shiftY,
        .centerDirection = {1.0, 0.0, 0.0},
        .surfaceFrame = {},
        .fullRefresh = fullRefresh
    });

    return update;
}
} // namespace

int main()
{
    const orbit::terrain_view::ClipmapConfig config{
        .levelCount = 1,
        .gridResolution = 9,
        .baseSpacingMeters = 1.0,
        .levelScale = 2.0,
        .overlapCells = 1
    };

    orbit::terrain_stream::ToroidalResidency residency(
        config);

    const auto initial =
        residency.Apply(
            MakeMotion(0, 0, true));

    if (initial.levels.size() != 1 ||
        !initial.levels[0].fullRefresh ||
        initial.levels[0].refreshRegions.size() != 1)
    {
        std::cerr << "Initial toroidal refresh is invalid.\n";
        return 1;
    }

    const auto& full =
        initial.levels[0].refreshRegions[0];

    if (full.x != 0 ||
        full.y != 0 ||
        full.width != 9 ||
        full.height != 9)
    {
        std::cerr << "Initial toroidal region is not the full window.\n";
        return 1;
    }

    const auto eastOne =
        residency.Apply(
            MakeMotion(1, 0, false));

    if (eastOne.levels[0].originX != 1 ||
        eastOne.levels[0].originY != 0 ||
        eastOne.levels[0].refreshRegions.size() != 1)
    {
        std::cerr << "One-cell east shift produced invalid residency state.\n";
        return 1;
    }

    const auto& eastStrip =
        eastOne.levels[0].refreshRegions[0];

    if (eastStrip.x != 0 ||
        eastStrip.y != 0 ||
        eastStrip.width != 1 ||
        eastStrip.height != 9)
    {
        std::cerr << "One-cell east shift refreshed the wrong physical strip.\n";
        return 1;
    }

    const auto eastTwo =
        residency.Apply(
            MakeMotion(2, 0, false));

    if (eastTwo.levels[0].originX != 3 ||
        eastTwo.levels[0].refreshRegions.size() != 1)
    {
        std::cerr << "Two-cell east shift produced invalid residency state.\n";
        return 1;
    }

    const auto& eastTwoStrip =
        eastTwo.levels[0].refreshRegions[0];

    if (eastTwoStrip.x != 1 ||
        eastTwoStrip.width != 2)
    {
        std::cerr << "Two-cell east shift refreshed the wrong strip.\n";
        return 1;
    }

    const auto westOne =
        residency.Apply(
            MakeMotion(-1, 0, false));

    if (westOne.levels[0].originX != 2 ||
        westOne.levels[0].refreshRegions.size() != 1)
    {
        std::cerr << "One-cell west shift produced invalid residency state.\n";
        return 1;
    }

    const auto& westStrip =
        westOne.levels[0].refreshRegions[0];

    if (westStrip.x != 2 ||
        westStrip.width != 1)
    {
        std::cerr << "One-cell west shift refreshed the wrong strip.\n";
        return 1;
    }

    residency.Reset();
    static_cast<void>(
        residency.Apply(
            MakeMotion(0, 0, true)));

    static_cast<void>(
        residency.Apply(
            MakeMotion(-1, 0, false)));

    const auto wrapped =
        residency.Apply(
            MakeMotion(2, 0, false));

    if (wrapped.levels[0].originX != 1 ||
        wrapped.levels[0].refreshRegions.size() != 2)
    {
        std::cerr << "Wrapped toroidal shift did not split its physical region.\n";
        return 1;
    }

    const auto& wrapA =
        wrapped.levels[0].refreshRegions[0];

    const auto& wrapB =
        wrapped.levels[0].refreshRegions[1];

    if (wrapA.x != 8 ||
        wrapA.width != 1 ||
        wrapB.x != 0 ||
        wrapB.width != 1)
    {
        std::cerr << "Wrapped toroidal physical regions are wrong.\n";
        return 1;
    }

    const auto north =
        residency.Apply(
            MakeMotion(0, 3, false));

    if (north.levels[0].originY != 3 ||
        north.levels[0].refreshRegions.empty())
    {
        std::cerr << "Vertical toroidal shift was not tracked.\n";
        return 1;
    }

    const auto huge =
        residency.Apply(
            MakeMotion(9, 0, false));

    if (!huge.levels[0].fullRefresh ||
        huge.levels[0].originX != 0 ||
        huge.levels[0].originY != 0 ||
        huge.levels[0].refreshRegions.size() != 1)
    {
        std::cerr << "Large toroidal shift did not force a full refresh.\n";
        return 1;
    }

    return 0;
}
