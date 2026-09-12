#include <orbit/terrain_hydrology/HydrologyGrid.hpp>

#include <cmath>
#include <iostream>

int main()
{
    orbit::terrain_hydrology::HydrologyGrid grid{};

    grid.config = {
        .resolution = 5,
        .halfExtentMeters = 2.0,
        .footprintMeters = 1.0,
        .useCoarseElevation = true
    };

    grid.spacingMeters = 1.0;
    grid.cells.resize(25);

    for (orbit::u32 y = 0;
         y < 5;
         ++y)
    {
        for (orbit::u32 x = 0;
             x < 5;
             ++x)
        {
            auto& cell =
                grid.At(
                    x,
                    y);

            cell.elevationMeters =
                static_cast<orbit::f32>(
                    std::abs(
                        static_cast<orbit::i32>(x) -
                        2) +
                    std::abs(
                        static_cast<orbit::i32>(y) -
                        2));

            cell.runoffWeight = 1.0F;
            cell.oceanWeight = 0.0F;
        }
    }

    orbit::terrain_hydrology::
        RouteHydrology(grid);

    const auto& center =
        grid.At(
            2,
            2);

    if (center.flowDx != 0 ||
        center.flowDy != 0)
    {
        std::cerr
            << "Hydrology basin center is not a sink.\n";
        return 1;
    }

    if (std::abs(
            center.flowAccumulation -
            25.0F) >
        1.0e-5F)
    {
        std::cerr
            << "Hydrology flow accumulation did not converge into the basin center.\n";
        return 1;
    }

    if (grid.At(0, 0).flowDx != 1 ||
        grid.At(0, 0).flowDy != 1)
    {
        std::cerr
            << "Hydrology D8 routing did not choose the steepest diagonal descent.\n";
        return 1;
    }

    const orbit::f64 centerArea =
        grid.DrainageAreaSquareMeters(
            2,
            2);

    if (std::abs(
            centerArea -
            25.0) >
        1.0e-9)
    {
        std::cerr
            << "Hydrology drainage-area conversion is wrong.\n";
        return 1;
    }

    const auto rivers =
        orbit::terrain_hydrology::
            ExtractRiverCells(
                grid,
                20.0);

    if (rivers.size() != 1 ||
        rivers[0] !=
            2U +
            2U * 5U)
    {
        std::cerr
            << "Hydrology river extraction did not isolate the basin outlet.\n";
        return 1;
    }

    return 0;
}
