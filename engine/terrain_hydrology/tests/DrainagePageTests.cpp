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

// Two pages that meet at a seam with a closed basin straddling it. The page
// service rebuilds each page whenever its neighbour's exported boundary
// changes, so the exchange must reach a fixed point instead of ping-ponging
// (conditioned heights creeping by the minimum drop and accumulated area
// re-imported through its own echo).
struct SeamExchange
{
    std::vector<DrainageBoundaryCell> leftEast;
    std::vector<DrainageBoundaryCell> rightWest;
    f64 totalOutflowArea{0.0};
    u32 iterations{0U};
    bool converged{false};
};

bool SameBoundary(
    const std::vector<DrainageBoundaryCell>& a,
    const std::vector<DrainageBoundaryCell>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0U; i < a.size(); ++i)
    {
        if (a[i].conditionedHeightMeters != b[i].conditionedHeightMeters ||
            a[i].drainageAreaSquareMeters != b[i].drainageAreaSquareMeters ||
            a[i].dischargeCubicMetersPerSecond != b[i].dischargeCubicMetersPerSecond ||
            a[i].flowDx != b[i].flowDx || a[i].flowDy != b[i].flowDy ||
            a[i].outlet != b[i].outlet ||
            a[i].basinTerminalFingerprint != b[i].basinTerminalFingerprint)
        {
            return false;
        }
    }
    return true;
}

SeamExchange RunSeamExchange(
    const bool simultaneous,
    const f32 centerHeight,
    const bool westOutlet = true,
    const bool eastOutlet = false,
    const f32 tilt = 0.0F)
{
    constexpr u32 resolution = 8U;
    constexpr u32 maxIterations = 40U;

    MaterialColumnPage left(resolution, 1.0);
    MaterialColumnPage right(resolution, 1.0);
    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            // Adjacent physical pages share their edge column (left x=7 is the
            // same physical point as right x=0), and the neighbour's copy of
            // that column is the halo. A bowl is centred on that shared column.
            const f32 leftDx = 7.0F - static_cast<f32>(x);
            const f32 rightDx = static_cast<f32>(x);
            const f32 dy = std::abs(static_cast<f32>(y) - 3.5F);
            left.SetCell(x, y, MakeCell(
                centerHeight + 0.6F * std::sqrt(leftDx * leftDx + dy * dy) - tilt * leftDx));
            right.SetCell(x, y, MakeCell(
                centerHeight + 0.6F * std::sqrt(rightDx * rightDx + dy * dy) + tilt * rightDx));
        }
    }

    const auto inputs = UniformInputs(resolution, 1.0F);
    DrainageRoutingConfig config{};
    config.minimumDrainageDropMeters = 0.01F;

    // Left page drains west over its rim to a low neighbour; everything else is
    // a wall, so the only way out of the joint basin is the left page's west edge.
    DrainagePageHalo leftHalo = MakeHalo(resolution, 100.0F, 100.0F, 100.0F, westOutlet ? -50.0F : 100.0F, 1U);
    DrainagePageHalo rightHalo = MakeHalo(resolution, 100.0F, eastOutlet ? -50.0F : 100.0F, 100.0F, 100.0F, 1U);

    std::vector<DrainageBoundaryCell> leftEast;
    std::vector<DrainageBoundaryCell> rightWest;
    SeamExchange result{};

    DrainagePage leftPage = BuildDrainagePage(left, MakeKey(resolution, 30U), inputs, leftHalo, config);
    DrainagePage rightPage = BuildDrainagePage(right, MakeKey(resolution, 30U), inputs, rightHalo, config);

    for (u32 iteration = 1U; iteration <= maxIterations; ++iteration)
    {
        // Neighbour boundary as published by the previous build, in the
        // receiving page's frame (both pages share an orientation here).
        std::vector<DrainageBoundaryCell> nextLeftEast;
        std::vector<DrainageBoundaryCell> nextRightWest;
        for (u32 i = 0U; i < resolution; ++i)
        {
            nextLeftEast.push_back(leftPage.BoundaryCell(DrainageBoundarySide::East, i));
            nextRightWest.push_back(rightPage.BoundaryCell(DrainageBoundarySide::West, i));
        }

        if (iteration > 1U &&
            SameBoundary(nextLeftEast, leftEast) &&
            SameBoundary(nextRightWest, rightWest))
        {
            result.converged = true;
            result.iterations = iteration;
            break;
        }
        leftEast = nextLeftEast;
        rightWest = nextRightWest;
        result.iterations = iteration;

        leftHalo.east = rightWest;
        leftHalo.revision = 1000U + iteration;
        DrainagePage nextLeft = BuildDrainagePage(left, MakeKey(resolution, 30U), inputs, leftHalo, config);

        rightHalo.west = simultaneous ? leftEast : std::vector<DrainageBoundaryCell>{};
        if (!simultaneous)
        {
            for (u32 i = 0U; i < resolution; ++i)
            {
                rightHalo.west.push_back(nextLeft.BoundaryCell(DrainageBoundarySide::East, i));
            }
        }
        rightHalo.revision = 2000U + iteration;
        DrainagePage nextRight = BuildDrainagePage(right, MakeKey(resolution, 30U), inputs, rightHalo, config);

        leftPage = std::move(nextLeft);
        rightPage = std::move(nextRight);
    }

    for (u32 y = 0U; y < resolution; ++y)
    {
        const DrainageCell& cell = leftPage.At(0U, y);
        if (cell.flow.exitsPage && cell.flow.dx < 0)
        {
            result.totalOutflowArea += cell.drainageAreaSquareMeters;
        }
    }
    result.leftEast = leftEast;
    result.rightWest = rightWest;
    return result;
}

void TestSeamBasinExchangeConverges()
{
    // Each page accumulates all 64 of its own 1 m cells (the shared column is
    // rained on by both copies), so nothing can legitimately exceed 128 m2.
    constexpr f64 totalArea = 2.0 * 8.0 * 8.0;

    for (const bool simultaneous : {true, false})
    {
        const SeamExchange exchange = RunSeamExchange(simultaneous, 5.0F);
        Require(
            exchange.converged,
            simultaneous
                ? "Simultaneous seam exchange around a joint basin never reached a fixed point."
                : "Sequential seam exchange around a joint basin never reached a fixed point.");
        Require(
            exchange.iterations <= 12U,
            "Seam exchange needed too many rebuild rounds to converge.");
        Require(
            exchange.totalOutflowArea <= totalArea + 1.0e-6,
            "The outlet received more area than both pages' cells hold: a "
            "seam contribution was counted again through its own echo.");
        Require(
            exchange.totalOutflowArea >= 64.0 - 1.0e-6,
            "The outlet must receive at least the outlet page's own rain.");
    }
}


// ---- 2x2 grid of seam-sharing pages with seeded random terrain ----------
struct GridExchange
{
    bool converged{false};
    u32 rounds{0U};
    // Accumulated area leaving the whole 2x2 domain through its outer edges.
    // Every page cell receives 1 m2 of rain, so a lossless exchange of the
    // four 9x9 pages exports 4 * 81 = 324 m2.
    f64 exitedArea{0.0};
};

f32 GridHeight(const u64 seed, const u32 gx, const u32 gy)
{
    // Smooth deterministic terrain on the global lattice, so shared columns
    // and corners carry identical raw heights in every page that has them.
    f64 h = 0.0;
    u64 state = seed * 0x9E3779B97F4A7C15ULL + 12345ULL;
    const auto next = [&state]()
    {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return static_cast<f64>(state % 100000ULL) / 100000.0;
    };
    for (u32 k = 0U; k < 5U; ++k)
    {
        const f64 a = 2.0 + 6.0 * next();
        const f64 fx = 0.15 + 0.5 * next();
        const f64 fy = 0.15 + 0.5 * next();
        const f64 px = 6.28 * next();
        const f64 py = 6.28 * next();
        h += a * std::sin(fx * gx + px) * std::sin(fy * gy + py);
    }
    return static_cast<f32>(20.0 + h);
}

GridExchange RunGridExchange(
    const u64 seed,
    const bool simultaneous)
{
    constexpr u32 resolution = 9U;
    constexpr u32 side = 2U;
    constexpr u32 maxRounds = 60U;

    struct Page
    {
        MaterialColumnPage material;
        DrainagePage drainage;
    };

    DrainageRoutingConfig config{};
    config.minimumDrainageDropMeters = 0.01F;
    const auto inputs = UniformInputs(resolution, 1.0F);

    std::vector<MaterialColumnPage> materials;
    materials.reserve(side * side);
    for (u32 py = 0U; py < side; ++py)
    {
        for (u32 px = 0U; px < side; ++px)
        {
            MaterialColumnPage material(resolution, 1.0);
            for (u32 y = 0U; y < resolution; ++y)
            {
                for (u32 x = 0U; x < resolution; ++x)
                {
                    material.SetCell(x, y, MakeCell(GridHeight(
                        seed, px * (resolution - 1U) + x, py * (resolution - 1U) + y)));
                }
            }
            materials.push_back(std::move(material));
        }
    }

    const auto outlet = []()
    {
        DrainageBoundaryCell cell = Boundary(-50.0F);
        return cell;
    };

    std::vector<std::optional<DrainagePage>> pages(side * side);
    const auto build = [&](const u32 px, const u32 py, const u64 revision)
    {
        // Shared-edge halo. Pages share their edge row/column, so the halo
        // holds the neighbour's cell one step BEYOND the shared edge
        // (north/east/south/west) and the neighbour's copy of the shared edge
        // itself (twin*). Sides with no neighbour are outlets.
        DrainagePageHalo halo{};
        halo.revision = revision;
        halo.north.assign(resolution, outlet());
        halo.east.assign(resolution, outlet());
        halo.south.assign(resolution, outlet());
        halo.west.assign(resolution, outlet());
        halo.twinNorth.assign(resolution, outlet());
        halo.twinEast.assign(resolution, outlet());
        halo.twinSouth.assign(resolution, outlet());
        halo.twinWest.assign(resolution, outlet());
        // Diagonal pages are not consulted (the Studio service cannot resolve
        // them across cube-face corners): corner ring cells are walls.
        const auto wall = []()
        {
            return Boundary(1.0e6F);
        };
        halo.corners = {wall(), wall(), wall(), wall()};
        halo.twinCorners = {outlet(), outlet(), outlet(), outlet()};

        const auto at = [&](const i32 qx, const i32 qy) -> const DrainagePage*
        {
            if (qx < 0 || qy < 0 || qx >= static_cast<i32>(side) || qy >= static_cast<i32>(side))
            {
                return nullptr;
            }
            const auto& page = pages[static_cast<std::size_t>(qy) * side + static_cast<u32>(qx)];
            return page.has_value() ? &*page : nullptr;
        };
        const i32 ix = static_cast<i32>(px);
        const i32 iy = static_cast<i32>(py);
        const u32 last = resolution - 1U;
        if (const auto* n = at(ix, iy - 1))
            for (u32 i = 0U; i < resolution; ++i)
            {
                halo.north[i] = n->CellAsBoundary(i, last - 1U);
                halo.twinNorth[i] = n->CellAsBoundary(i, last);
            }
        if (const auto* e = at(ix + 1, iy))
            for (u32 i = 0U; i < resolution; ++i)
            {
                halo.east[i] = e->CellAsBoundary(1U, i);
                halo.twinEast[i] = e->CellAsBoundary(0U, i);
            }
        if (const auto* so = at(ix, iy + 1))
            for (u32 i = 0U; i < resolution; ++i)
            {
                halo.south[i] = so->CellAsBoundary(i, 1U);
                halo.twinSouth[i] = so->CellAsBoundary(i, 0U);
            }
        if (const auto* w = at(ix - 1, iy))
            for (u32 i = 0U; i < resolution; ++i)
            {
                halo.west[i] = w->CellAsBoundary(last - 1U, i);
                halo.twinWest[i] = w->CellAsBoundary(last, i);
            }

        return BuildDrainagePage(
            materials[static_cast<std::size_t>(py) * side + px],
            MakeKey(resolution, 40U),
            inputs,
            halo,
            config);
    };

    const auto fingerprint = [&]()
    {
        std::vector<DrainageBoundaryCell> all;
        for (const auto& page : pages)
        {
            for (const auto sideId : {DrainageBoundarySide::North, DrainageBoundarySide::East,
                                      DrainageBoundarySide::South, DrainageBoundarySide::West})
            {
                for (u32 i = 0U; i < resolution; ++i)
                {
                    all.push_back(page->BoundaryCell(sideId, i));
                }
            }
        }
        return all;
    };

    // Initial independent build of every page.
    for (u32 py = 0U; py < side; ++py)
        for (u32 px = 0U; px < side; ++px)
            pages[py * side + px] = build(px, py, 1U);

    GridExchange result{};
    auto previous = fingerprint();
    for (u32 round = 1U; round <= maxRounds; ++round)
    {
        if (simultaneous)
        {
            std::vector<std::optional<DrainagePage>> next(side * side);
            for (u32 py = 0U; py < side; ++py)
                for (u32 px = 0U; px < side; ++px)
                    next[py * side + px] = build(px, py, 100U + round);
            pages = std::move(next);
        }
        else
        {
            for (u32 py = 0U; py < side; ++py)
                for (u32 px = 0U; px < side; ++px)
                    pages[py * side + px] = build(px, py, 100U + round);
        }

        const auto now = fingerprint();
        result.rounds = round;
        result.exitedArea = 0.0;
        for (u32 py = 0U; py < side; ++py)
        {
            for (u32 px = 0U; px < side; ++px)
            {
                const DrainagePage& page = *pages[py * side + px];
                for (u32 y = 0U; y < resolution; ++y)
                {
                    for (u32 x = 0U; x < resolution; ++x)
                    {
                        const DrainageCell& cell = page.At(x, y);
                        const i32 gx = static_cast<i32>(px * (resolution - 1U) + x) + cell.flow.dx;
                        const i32 gy = static_cast<i32>(py * (resolution - 1U) + y) + cell.flow.dy;
                        constexpr i32 last = static_cast<i32>(side * (resolution - 1U));
                        if (cell.flow.exitsPage && (gx < 0 || gy < 0 || gx > last || gy > last))
                        {
                            result.exitedArea += cell.drainageAreaSquareMeters;
                        }
                    }
                }
            }
        }
        if (SameBoundary(now, previous))
        {
            result.converged = true;
            break;
        }
        previous = now;
    }
    return result;
}

// The cross-page seam exchange over a 2x2 grid of seam-sharing pages with
// seeded random terrain. Pages are rebuilt from each other's published
// boundaries until the exported boundaries stop changing.
//
// The rebuilds are sequential, matching StudioTerrainRebuildScheduler's page
// gate, which never builds two edge-adjacent pages at once. Rebuilding
// adjacent pages simultaneously from the same stale snapshot can still swap
// states forever (the exchange is bistable around lakes that span a seam), so
// the sequential order is part of the contract, not an implementation detail.
// The exchange must reach a fixed point and be lossless: every page cell
// receives 1 m2 of rain and all of it must leave the domain.
void TestGridSeamExchangeConverges()
{
    constexpr u32 seeds = 60U;
    for (u32 seed = 1U; seed <= seeds; ++seed)
    {
        const GridExchange exchange = RunGridExchange(seed, false);
        Require(
            exchange.converged,
            "Sequential seam exchange over a random 2x2 page grid never reached a fixed point.");
        RequireNear(
            exchange.exitedArea,
            324.0,
            1.0e-6,
            "Seam exchange lost or invented drainage area: all rain on the 2x2 page "
            "grid must leave through its outer edges exactly once.");
    }
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
    TestSeamBasinExchangeConverges();
    TestGridSeamExchangeConverges();
    TestFillRejectsMissingHalo();

    std::cout << "Orbit M09 drainage-page tests passed.\n";
    return EXIT_SUCCESS;
}
