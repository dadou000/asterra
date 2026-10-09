#include "BakedRiverPage.hpp"

#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/world/Planet.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <set>
#include <vector>

namespace
{
using namespace orbit;

constexpr u32 kResolution = 33U;
constexpr f64 kSpacing = 100.0;
constexpr u8 kLevel = 8U;

bool Check(const bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

// A planet whose level-8 tile is exactly as wide as the page, so page metres
// and sphere metres agree.
f64 PlanetRadius()
{
    return kSpacing * (kResolution - 1U) * 256.0 / (std::numbers::pi * 0.5);
}

world::PlanetTileId Tile()
{
    return {.face = world::CubeFace::PositiveZ, .level = kLevel, .x = 128, .y = 128};
}

math::Double3 AtUv(const world::CubeBounds& b, const f64 fx, const f64 fy)
{
    return world::CubeToUnitDirection({
        .face = b.face,
        .uv = {b.minimumUv.x + (b.maximumUv.x - b.minimumUv.x) * fx,
               b.minimumUv.y + (b.maximumUv.y - b.minimumUv.y) * fy}});
}

terrain_hydrology::DrainagePage BuildPage(f64 runoff)
{
    terrain_material_column::MaterialColumnPage material(kResolution, kSpacing);
    for (u32 y = 0; y < kResolution; ++y)
    {
        for (u32 x = 0; x < kResolution; ++x)
        {
            const f32 height = 100.0F - 0.2F * static_cast<f32>(y);
            material.SetCell(x, y, {
                .bedrockHeightMeters = height,
                .referenceBedrockHeightMeters = height,
                .bedrockMaterial = terrain_geology::reference_rock::Basalt});
        }
    }

    terrain::PhysicalTerrainPageKey key{};
    key.address.tile = Tile();
    key.resolution = kResolution;
    key.revisions.geology = 1U;

    std::vector<terrain_hydrology::DrainageCellInput> inputs(
        static_cast<std::size_t>(kResolution) * kResolution,
        {.runoffMetersPerSecond = static_cast<f32>(runoff)});
    terrain_hydrology::DrainageRoutingConfig config{};
    config.depressionPolicy = terrain_hydrology::DepressionRoutingPolicy::PreserveClosed;
    return terrain_hydrology::BuildDrainagePage(material, key, inputs, {}, config);
}

terrain::BakedRiverNetwork River(const world::CubeBounds& bounds)
{
    std::vector<terrain::BakedRiverNode> nodes(3);
    // Enters the page from outside on the north edge, crosses it, leaves south.
    nodes[0].direction = AtUv(bounds, 0.5, -0.6);
    nodes[1].direction = AtUv(bounds, 0.5, 0.5);
    nodes[2].direction = AtUv(bounds, 0.5, 1.8);
    for (u32 i = 0; i < 3; ++i)
    {
        nodes[i].widthMeters = 400.0F;
        nodes[i].depthMeters = 6.0F;
        nodes[i].dischargeCubicMetersPerSecond = 5000.0F + 1000.0F * static_cast<f32>(i);
        nodes[i].basin = 2;
    }
    return terrain::BakedRiverNetwork::Build(PlanetRadius(), 1, nodes, {{0, 1}, {1, 2}});
}
} // namespace

int main()
{
    const auto bounds = world::TileBounds(Tile());
    const auto network = River(bounds);
    bool ok = true;

    auto page = BuildPage(1.0e-8);
    std::vector<terrain_hydrology::DrainageCellInput> inputs(
        static_cast<std::size_t>(kResolution) * kResolution,
        {.runoffMetersPerSecond = 1.0e-8F});

    const f64 localCenter = page.At(16, 16).dischargeCubicMetersPerSecond;
    const f64 localCorner = page.At(2, 2).dischargeCubicMetersPerSecond;
    studio_session::ApplyBakedRiverDischarge(page, network, inputs);
    ok &= Check(page.At(16, 16).dischargeCubicMetersPerSecond >= 5000.0,
        "cells on the baked channel take the baked discharge");
    ok &= Check(page.At(16, 16).dischargeCubicMetersPerSecond > localCenter,
        "the baked discharge raises the local one");
    ok &= Check(page.At(2, 2).dischargeCubicMetersPerSecond == localCorner,
        "cells away from the channel keep their local discharge");
    ok &= Check(page.At(16, 16).drainageAreaSquareMeters >= 5000.0 / 1.0e-8 * 0.999,
        "area follows the discharge through the runoff rate");

    const auto rivers = studio_session::BuildBakedPageRiverNetwork(network, page, 500.0);
    ok &= Check(!rivers.nodes.empty(), "the channel crossing the page becomes a page network");
    ok &= Check(rivers.segments.size() + 1U == rivers.nodes.size(),
        "one connected chain of nodes and segments");
    const f64 half = 0.5 * (kResolution - 1U) * kSpacing;
    std::set<std::pair<u64, u64>> ids;
    for (const auto& node : rivers.nodes)
    {
        ok &= Check(std::abs(node.channelOffsetMeters.x) <= half + 1.0e-6 &&
                        std::abs(node.channelOffsetMeters.y) <= half + 1.0e-6,
            "nodes stay inside the page");
        ok &= Check(node.dischargeCubicMetersPerSecond >= 5000.0 &&
                        node.dischargeCubicMetersPerSecond <= 7000.0,
            "node discharge interpolates the baked reach");
        ok &= Check(std::abs(node.channelOffsetMeters.x) < 50.0, "the channel runs down the page centre");
        ids.insert({node.id.high, node.id.low});
    }
    ok &= Check(ids.size() == rivers.nodes.size(), "node ids are unique");
    ok &= Check(rivers.nodes.back().exitsPage && rivers.boundaryLinks.size() == 1U,
        "the downstream end leaves the page with a boundary link");
    ok &= Check(!rivers.nodes.front().exitsPage, "the entry node does not exit");
    for (std::size_t i = 0; i + 1 < rivers.nodes.size(); ++i)
    {
        ok &= Check(rivers.nodes[i + 1].channelOffsetMeters.y > rivers.nodes[i].channelOffsetMeters.y,
            "nodes are ordered downstream (south)");
    }
    ok &= Check(!rivers.basins.empty() && rivers.basins.front().nodeCount == rivers.nodes.size(),
        "one basin owns the chain");

    const auto again = studio_session::BuildBakedPageRiverNetwork(network, page, 500.0);
    ok &= Check(again.nodes.size() == rivers.nodes.size() &&
                    again.nodes.front().id == rivers.nodes.front().id,
        "extraction is deterministic");

    // A reach entirely outside the page contributes nothing.
    std::vector<terrain::BakedRiverNode> far(2);
    far[0].direction = AtUv(bounds, 3.0, 3.0);
    far[1].direction = AtUv(bounds, 4.0, 3.0);
    for (auto& node : far)
    {
        node.widthMeters = 400.0F;
        node.depthMeters = 6.0F;
        node.dischargeCubicMetersPerSecond = 5000.0F;
    }
    const auto outside = terrain::BakedRiverNetwork::Build(PlanetRadius(), 2, far, {{0, 1}});
    ok &= Check(studio_session::BuildBakedPageRiverNetwork(outside, page, 500.0).nodes.empty(),
        "reaches outside the page are not included");

    const auto empty = terrain::BakedRiverNetwork::Build(PlanetRadius(), 3, {}, {});
    ok &= Check(studio_session::BuildBakedPageRiverNetwork(empty, page, 500.0).nodes.empty(),
        "an empty bake gives an empty network");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
