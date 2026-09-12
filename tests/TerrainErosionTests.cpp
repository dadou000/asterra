#include <orbit/terrain_erosion/RiverCarving.hpp>

#include <cmath>
#include <iostream>

namespace
{
bool NearlyEqual(
    const orbit::f64 a,
    const orbit::f64 b,
    const orbit::f64 epsilon)
{
    return
        std::abs(a - b) <=
        epsilon;
}
} // namespace

int main()
{
    orbit::terrain_hydrology::HydrologyGrid
        hydrology{};

    hydrology.config = {
        .resolution = 9,
        .halfExtentMeters = 4'000.0
    };

    hydrology.spacingMeters = 1'000.0;

    orbit::terrain_hydrology::RiverGraph
        rivers{};

    rivers.nodes = {
        {
            .sourceCellIndex = 0,
            .offsetMeters = {
                0.0,
                0.0
            },
            .elevationMeters = 100.0F,
            .drainageElevationMeters = 100.0F,
            .depressionFillMeters = 0.0F,
            .drainageAreaSquareMeters =
                1'000'000.0
        },
        {
            .sourceCellIndex = 1,
            .offsetMeters = {
                1'000.0,
                0.0
            },
            .elevationMeters = 99.0F,
            .drainageElevationMeters = 90.0F,
            .depressionFillMeters = 0.0F,
            .drainageAreaSquareMeters =
                4'000'000.0
        },
        {
            .sourceCellIndex = 2,
            .offsetMeters = {
                2'000.0,
                0.0
            },
            .elevationMeters = 98.0F,
            .drainageElevationMeters = 80.0F,
            .depressionFillMeters = 0.0F,
            .drainageAreaSquareMeters =
                16'000'000.0
        }
    };

    rivers.segments = {
        {
            .upstreamNode = 0,
            .downstreamNode = 1
        },
        {
            .upstreamNode = 1,
            .downstreamNode = 2
        }
    };

    const auto field =
        orbit::terrain_erosion::
            BuildRiverCarvingField(
                hydrology,
                rivers,
                {
                    .referenceDrainageAreaSquareMeters =
                        1'000'000.0,
                    .baseChannelHalfWidthMeters =
                        5.0,
                    .minimumChannelHalfWidthMeters =
                        2.0,
                    .maximumChannelHalfWidthMeters =
                        100.0,
                    .widthExponent =
                        0.35,
                    .baseDepthMeters =
                        3.0,
                    .minimumDepthMeters =
                        1.0,
                    .maximumDepthMeters =
                        30.0,
                    .depthExponent =
                        0.20,
                    .valleyWidthMultiplier =
                        5.0,
                    .minimumBedSlope =
                        0.01,
                    .maximumIncisionMeters =
                        100.0
                });

    if (field.nodes.size() != 3 ||
        field.segments.size() != 2)
    {
        std::cerr
            << "River carving field topology does not match the river graph.\n";
        return 1;
    }

    if (!(field.nodes[2].
              channelHalfWidthMeters >
          field.nodes[1].
              channelHalfWidthMeters &&
          field.nodes[1].
              channelHalfWidthMeters >
          field.nodes[0].
              channelHalfWidthMeters))
    {
        std::cerr
            << "River channel width does not grow with drainage area.\n";
        return 1;
    }

    for (const auto& segment :
         field.segments)
    {
        const auto& upstream =
            field.nodes[
                segment.upstreamNode];

        const auto& downstream =
            field.nodes[
                segment.downstreamNode];

        const orbit::f64 dx =
            upstream.offsetMeters.x -
            downstream.offsetMeters.x;

        const orbit::f64 dy =
            upstream.offsetMeters.y -
            downstream.offsetMeters.y;

        const orbit::f64 length =
            std::sqrt(
                dx * dx +
                dy * dy);

        const orbit::f64 requiredDrop =
            0.01 *
            length;

        const orbit::f64 actualDrop =
            upstream.
                bedElevationMeters -
            downstream.
                bedElevationMeters;

        if (actualDrop + 1.0e-4 <
            requiredDrop)
        {
            std::cerr
                << "River bed profile does not satisfy the configured minimum slope.\n";
            return 1;
        }
    }

    for (const auto& node :
         field.nodes)
    {
        if (node.bedElevationMeters >
                node.sourceElevationMeters ||
            node.bedElevationMeters <
                node.sourceElevationMeters -
                    100.0F -
                    1.0e-4F)
        {
            std::cerr
                << "River bed profile violated the incision bounds.\n";
            return 1;
        }
    }

    const auto center =
        orbit::terrain_erosion::
            SampleRiverCarving(
                field,
                {
                    500.0,
                    0.0
                });

    if (!center.active ||
        !NearlyEqual(
            center.distanceToCenterMeters,
            0.0,
            1.0e-9) ||
        center.influence <
            0.999)
    {
        std::cerr
            << "River carving centerline sample is invalid.\n";
        return 1;
    }

    const orbit::f64 expectedCenterBed =
        0.5 *
        (static_cast<orbit::f64>(
             field.nodes[0].
                 bedElevationMeters) +
         static_cast<orbit::f64>(
             field.nodes[1].
                 bedElevationMeters));

    if (!NearlyEqual(
            center.targetElevationMeters,
            expectedCenterBed,
            1.0e-4))
    {
        std::cerr
            << "River carving centerline did not interpolate the bed profile.\n";
        return 1;
    }

    const auto outside =
        orbit::terrain_erosion::
            SampleRiverCarving(
                field,
                {
                    500.0,
                    1'000.0
                });

    if (outside.active)
    {
        std::cerr
            << "River carving affected terrain outside the valley width.\n";
        return 1;
    }

    const orbit::f64 valleyWidth =
        0.5 *
        (static_cast<orbit::f64>(
             field.nodes[0].
                 valleyHalfWidthMeters) +
         static_cast<orbit::f64>(
             field.nodes[1].
                 valleyHalfWidthMeters));

    const auto bank =
        orbit::terrain_erosion::
            SampleRiverCarving(
                field,
                {
                    500.0,
                    valleyWidth *
                        0.8
                });

    if (!bank.active ||
        bank.influence <= 0.0 ||
        bank.influence >= 1.0 ||
        bank.targetElevationMeters <=
            center.targetElevationMeters)
    {
        std::cerr
            << "River carving bank transition is not smooth.\n";
        return 1;
    }

    return 0;
}
