#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace orbit;
using namespace orbit::terrain_hydrology;
using namespace orbit::terrain_material_column;

[[noreturn]] void Fail(const std::string& message)
{
    std::cerr << "M09 failure: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void Require(
    const bool condition,
    const std::string& message)
{
    if (!condition)
    {
        Fail(message);
    }
}

void RequireNear(
    const f64 a,
    const f64 b,
    const f64 tolerance,
    const std::string& message)
{
    if (std::abs(a - b) > tolerance)
    {
        Fail(
            message + " (" +
            std::to_string(a) + " vs " +
            std::to_string(b) + ")");
    }
}

MaterialColumnCell MakeCell(
    const f32 bedrockHeightMeters)
{
    return {
        .bedrockHeightMeters =
            bedrockHeightMeters,
        .referenceBedrockHeightMeters =
            bedrockHeightMeters,
        .bedrockMaterial =
            terrain_geology::reference_rock::Basalt,
        .regolithMeters = 0.0F,
        .soilMeters = 0.0F,
        .sandMeters = 0.0F,
        .debrisMeters = 0.0F,
        .moisture = 0.0F,
        .temporaryScalar = 0.0F
    };
}

terrain::PhysicalTerrainPageKey MakeKey(
    const u32 resolution,
    const u64 processRevision = 1U)
{
    terrain::PhysicalTerrainPageKey key{};
    key.resolution = resolution;
    key.revisions.geology = 3U;
    key.revisions.climate = 5U;
    key.revisions.authoring = 7U;
    key.revisions.processes =
        processRevision;
    return key;
}

DrainageBoundaryCell Boundary(
    const f32 height)
{
    return {
        .surfaceHeightMeters = height,
        .conditionedHeightMeters = height,
        .authoredDrainage = 0.0F,
        .drainageAreaSquareMeters = 0.0,
        .dischargeCubicMetersPerSecond = 0.0,
        .flowDx = 0,
        .flowDy = 0
    };
}

DrainagePageHalo MakeHalo(
    const u32 resolution,
    const f32 northHeight,
    const f32 eastHeight,
    const f32 southHeight,
    const f32 westHeight,
    const u64 revision = 1U)
{
    DrainagePageHalo halo{};
    halo.revision = revision;

    halo.north.assign(
        resolution,
        Boundary(northHeight));

    halo.east.assign(
        resolution,
        Boundary(eastHeight));

    halo.south.assign(
        resolution,
        Boundary(southHeight));

    halo.west.assign(
        resolution,
        Boundary(westHeight));

    const f32 cornerHeight =
        std::max(
            std::max(
                northHeight,
                southHeight),
            std::max(
                eastHeight,
                westHeight));

    halo.corners = {
        Boundary(cornerHeight),
        Boundary(cornerHeight),
        Boundary(cornerHeight),
        Boundary(cornerHeight)
    };

    return halo;
}

std::vector<DrainageCellInput> UniformInputs(
    const u32 resolution,
    const f32 runoffMetersPerSecond = 0.0F)
{
    return std::vector<DrainageCellInput>(
        static_cast<std::size_t>(
            resolution) *
            resolution,
        DrainageCellInput{
            .runoffMetersPerSecond =
                runoffMetersPerSecond,
            .authoredDrainage = 0.0F,
            .outlet = false
        });
}

void TestUsesM08PhysicalSurface()
{
    constexpr u32 resolution = 3U;

    MaterialColumnPage material(
        resolution,
        2.0);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            material.SetCell(
                x,
                y,
                MakeCell(10.0F));
        }
    }

    material.At(1, 1).regolithMeters =
        1.0F;
    material.At(1, 1).soilMeters =
        2.0F;
    material.At(1, 1).sandMeters =
        0.5F;
    material.At(1, 1).debrisMeters =
        0.25F;

    auto inputs =
        UniformInputs(
            resolution);

    DrainageRoutingConfig config{};
    config.depressionPolicy =
        DepressionRoutingPolicy::
            PreserveClosed;

    const DrainagePage drainage =
        BuildDrainagePage(
            material,
            MakeKey(resolution),
            inputs,
            {},
            config);

    RequireNear(
        drainage.At(1, 1).
            surfaceHeightMeters,
        13.75,
        1.0e-5,
        "M09 must route over the complete M08 physical surface, not "
        "bedrock height alone.");
}

void TestSelectedDepressionPolicy()
{
    constexpr u32 resolution = 5U;

    MaterialColumnPage material(
        resolution,
        1.0);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            material.SetCell(
                x,
                y,
                MakeCell(
                    x == 2U &&
                            y == 2U
                        ? 0.0F
                        : 10.0F));
        }
    }

    auto inputs =
        UniformInputs(
            resolution,
            0.001F);

    const DrainagePageHalo halo =
        MakeHalo(
            resolution,
            0.0F,
            0.0F,
            0.0F,
            0.0F);

    DrainageRoutingConfig preserve{};
    preserve.depressionPolicy =
        DepressionRoutingPolicy::
            PreserveClosed;

    const DrainagePage closed =
        BuildDrainagePage(
            material,
            MakeKey(resolution),
            inputs,
            halo,
            preserve);

    Require(
        !closed.At(2, 2).
            flow.HasDownstream(),
        "PreserveClosed must retain a real closed sink.");

    DrainageRoutingConfig fill{};
    fill.depressionPolicy =
        DepressionRoutingPolicy::
            FillToBoundary;
    fill.minimumDrainageDropMeters =
        0.1F;

    const DrainagePage routed =
        BuildDrainagePage(
            material,
            MakeKey(resolution),
            inputs,
            halo,
            fill);

    Require(
        routed.At(2, 2).
            depressionFillMeters >
            9.0F,
        "FillToBoundary must raise the derived routing surface across "
        "a closed depression.");

    Require(
        routed.At(2, 2).
            flow.HasDownstream(),
        "Filled depression must have a deterministic downhill route.");
}

void TestGuidanceCannotCreateUphillFlow()
{
    constexpr u32 resolution = 3U;

    MaterialColumnPage material(
        resolution,
        1.0);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            material.SetCell(
                x,
                y,
                MakeCell(
                    10.0F -
                    static_cast<f32>(x)));
        }
    }

    auto inputs =
        UniformInputs(
            resolution);

    // Strong positive guidance on the uphill west neighbor must not make it
    // eligible. Guidance only ranks already-downhill candidates.
    inputs[1U * resolution + 0U].
        authoredDrainage = 1.0F;

    inputs[1U * resolution + 2U].
        authoredDrainage = -1.0F;

    DrainageRoutingConfig config{};
    config.depressionPolicy =
        DepressionRoutingPolicy::
            PreserveClosed;
    config.authoredGuidanceWeight =
        0.95F;

    const auto halo =
        MakeHalo(
            resolution,
            100.0F,
            6.0F,
            100.0F,
            12.0F);

    const DrainagePage drainage =
        BuildDrainagePage(
            material,
            MakeKey(resolution),
            inputs,
            halo,
            config);

    const DrainageCell& center =
        drainage.At(1, 1);

    Require(
        center.flow.dx == 1,
        "M04 drainage guidance must remain an input preference and "
        "must not make an uphill neighbor physically routable.");
}

void TestDeterministicRevisionAndFlow()
{
    constexpr u32 resolution = 4U;

    MaterialColumnPage material(
        resolution,
        3.0);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            material.SetCell(
                x,
                y,
                MakeCell(
                    20.0F -
                    static_cast<f32>(
                        x + y)));
        }
    }

    auto inputs =
        UniformInputs(
            resolution,
            0.0002F);

    const auto halo =
        MakeHalo(
            resolution,
            30.0F,
            10.0F,
            10.0F,
            30.0F,
            41U);

    DrainageRoutingConfig config{};
    config.minimumDrainageDropMeters =
        0.05F;

    const auto key =
        MakeKey(
            resolution,
            11U);

    const DrainagePage a =
        BuildDrainagePage(
            material,
            key,
            inputs,
            halo,
            config);

    const DrainagePage b =
        BuildDrainagePage(
            material,
            key,
            inputs,
            halo,
            config);

    Require(
        a.Revision() == b.Revision(),
        "Identical M09 physical inputs must produce identical drainage "
        "revision identity.");

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            const DrainageCell& lhs =
                a.At(x, y);

            const DrainageCell& rhs =
                b.At(x, y);

            Require(
                lhs.surfaceHeightMeters ==
                    rhs.surfaceHeightMeters &&
                lhs.drainageElevationMeters ==
                    rhs.drainageElevationMeters &&
                lhs.depressionFillMeters ==
                    rhs.depressionFillMeters &&
                lhs.drainageAreaSquareMeters ==
                    rhs.drainageAreaSquareMeters &&
                lhs.dischargeCubicMetersPerSecond ==
                    rhs.dischargeCubicMetersPerSecond &&
                lhs.flow.dx == rhs.flow.dx &&
                lhs.flow.dy == rhs.flow.dy &&
                lhs.flow.exitsPage ==
                    rhs.flow.exitsPage,
                "M09 flow result is not deterministic.");
        }
    }

    const u64 processChanged =
        DrainageRevisionFingerprint(
            MakeKey(
                resolution,
                12U),
            config,
            halo.revision);

    Require(
        processChanged !=
            a.Revision(),
        "M08/process revision changes must invalidate M09 drainage.");

    const u64 boundaryChanged =
        DrainageRevisionFingerprint(
            key,
            config,
            halo.revision + 1U);

    Require(
        boundaryChanged !=
            a.Revision(),
        "Neighbor drainage revision changes must invalidate M09.");
}

void TestCrossPageBoundaryExchange()
{
    constexpr u32 resolution = 4U;

    MaterialColumnPage left(
        resolution,
        1.0);

    MaterialColumnPage right(
        resolution,
        1.0);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            left.SetCell(
                x,
                y,
                MakeCell(
                    8.0F -
                    static_cast<f32>(x)));

            right.SetCell(
                x,
                y,
                MakeCell(
                    4.0F -
                    static_cast<f32>(x)));
        }
    }

    auto inputs =
        UniformInputs(
            resolution,
            1.0F);

    DrainageRoutingConfig config{};
    config.minimumDrainageDropMeters =
        0.01F;

    DrainagePageHalo leftHalo =
        MakeHalo(
            resolution,
            100.0F,
            4.0F,
            100.0F,
            9.0F,
            100U);

    const DrainagePage leftDrainage =
        BuildDrainagePage(
            left,
            MakeKey(resolution, 20U),
            inputs,
            leftHalo,
            config);

    const DrainageCell& leftEdge =
        leftDrainage.At(
            resolution - 1U,
            1U);

    Require(
        leftEdge.flow.exitsPage &&
        leftEdge.flow.dx == 1,
        "A downhill physical seam must route across the adjacent page "
        "instead of becoming a page-edge sink.");

    DrainagePageHalo rightHalo =
        MakeHalo(
            resolution,
            100.0F,
            0.0F,
            100.0F,
            5.0F,
            101U);

    for (u32 y = 0; y < resolution; ++y)
    {
        rightHalo.west[y] =
            leftDrainage.
                BoundaryCell(
                    DrainageBoundarySide::
                        East,
                    y);

        // In the receiving page's local orientation the imported source sits
        // one cell west of x=0 and drains east into the first interior cell.
        rightHalo.west[y].flowDx = 1;
        rightHalo.west[y].flowDy = 0;
    }

    const DrainagePage rightDrainage =
        BuildDrainagePage(
            right,
            MakeKey(resolution, 20U),
            inputs,
            rightHalo,
            config);

    const f64 ownArea =
        right.CellAreaSquareMeters();

    RequireNear(
        leftEdge.
            drainageAreaSquareMeters,
        ownArea *
            static_cast<f64>(
                resolution),
        1.0e-9,
        "Left-page row accumulation did not reach the seam.");

    RequireNear(
        rightDrainage.At(0, 1).
            drainageAreaSquareMeters,
        leftEdge.
                drainageAreaSquareMeters +
            ownArea,
        1.0e-9,
        "Cross-page contributing area was not injected into the "
        "receiving page.");

    RequireNear(
        rightDrainage.At(
            resolution - 1U,
            1U).
            drainageAreaSquareMeters,
        ownArea *
            static_cast<f64>(
                resolution * 2U),
        1.0e-9,
        "Drainage area must remain continuous after crossing a page "
        "seam.");

    RequireNear(
        rightDrainage.At(
            resolution - 1U,
            1U).
            dischargeCubicMetersPerSecond,
        ownArea *
            static_cast<f64>(
                resolution * 2U),
        1.0e-9,
        "Discharge must remain continuous after cross-page boundary "
        "exchange.");
}

void TestFillRejectsMissingHalo()
{
    constexpr u32 resolution = 3U;

    MaterialColumnPage material(
        resolution,
        1.0);

    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            material.SetCell(
                x,
                y,
                MakeCell(1.0F));
        }
    }

    const auto inputs =
        UniformInputs(
            resolution);

    bool rejected = false;

    try
    {
        (void)BuildDrainagePage(
            material,
            MakeKey(resolution),
            inputs,
            {});
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }

    Require(
        rejected,
        "FillToBoundary must reject missing neighbor state instead of "
        "silently turning a page edge into an outlet.");
}
} // namespace

int main()
{
    TestUsesM08PhysicalSurface();
    TestSelectedDepressionPolicy();
    TestGuidanceCannotCreateUphillFlow();
    TestDeterministicRevisionAndFlow();
    TestCrossPageBoundaryExchange();
    TestFillRejectsMissingHalo();

    std::cout << "Orbit M09 drainage-page tests passed.\n";
    return EXIT_SUCCESS;
}
