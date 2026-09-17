from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEST = ROOT / "tests" / "V003ScaffoldIntegrationTests.cpp"
CMAKE = ROOT / "tests" / "CMakeLists.txt"
SPEC = ROOT / "docs" / "V0.0.3_SPEC.md"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


def main() -> None:
    test = TEST.read_text(encoding="utf-8")

    test = replace_once(
        test,
        "#include <orbit/selection/SelectionService.hpp>\n#include <orbit/time/SimulationTime.hpp>\n",
        "#include <orbit/selection/SelectionService.hpp>\n#include <orbit/surface/SurfaceRegistry.hpp>\n#include <orbit/terrain/AnalyticTerrainSource.hpp>\n#include <orbit/time/SimulationTime.hpp>\n",
        "terrain proof includes",
    )
    test = replace_once(
        test,
        "#include <iterator>\n#include <source_location>\n",
        "#include <iterator>\n#include <memory>\n#include <source_location>\n",
        "memory include",
    )

    marker = '''        const auto* primaryBody =
            bodies.FindBody(primary);
        Check(primaryBody != nullptr);

        const auto movingBody =
'''
    replacement = '''        const auto* primaryBody =
            bodies.FindBody(primary);
        Check(primaryBody != nullptr);

        // M23 must prove that the existing rocky-planet terrain stack is a
        // capability of a celestial body, not a parallel global planet.
        orbit::surface::SurfaceRegistry surfaces(bodies);
        auto terrainSource =
            std::make_shared<orbit::terrain::AnalyticTerrainSource>(
                orbit::world::PlanetDefinition{
                    .radiusMeters = 6'000'000.0
                });
        surfaces.AttachTerrain(primary, terrainSource);

        const auto* terrainCapability =
            surfaces.FindTerrainSurface(primary);
        Check(terrainCapability != nullptr);
        Check(terrainCapability->terrain == terrainSource);

        const auto terrainPlanet =
            surfaces.SphericalPlanetDefinition(primary);
        Check(terrainPlanet.has_value());
        Check(terrainPlanet->radiusMeters == 6'000'000.0);

        const orbit::terrain::TerrainSample terrainSample =
            terrainCapability->terrain->Sample({
                .unitDirection = {0.0, 1.0, 0.0},
                .footprintMeters = 100.0
            });
        Check(std::isfinite(terrainSample.elevationMeters));

        const auto movingBody =
'''
    test = replace_once(
        test,
        marker,
        replacement,
        "terrain capability proof",
    )
    test = replace_once(
        test,
        "#include <array>\n#include <cstdlib>\n",
        "#include <array>\n#include <cmath>\n#include <cstdlib>\n",
        "cmath include",
    )
    TEST.write_text(test, encoding="utf-8", newline="\n")

    cmake = CMAKE.read_text(encoding="utf-8")
    cmake = replace_once(
        cmake,
        "        Orbit::Plugins\n        Orbit::Universe\n",
        "        Orbit::Plugins\n        Orbit::Surface\n        Orbit::Terrain\n        Orbit::Universe\n",
        "M23 terrain links",
    )
    CMAKE.write_text(cmake, encoding="utf-8", newline="\n")

    spec = SPEC.read_text(encoding="utf-8")
    spec = replace_once(
        spec,
        "| M23 — V0.0.3 integration proof | **Partial — acceptance running** | `OrbitV003ScaffoldIntegrationTests` now generates one real project and exercises save/reopen, two moving celestial frames, an external Luau plugin, JSON-RPC/MCP-equivalent editing, PBR/decal authoring with undo/redo, Direct/Bézier/Routed paths, Steam mapping validation, actual `OrbitPlayer` packaging, and deterministic rebuild after deleting DDC. Studio also owns two independently resizable GPU RenderViews. Final status closes only after the Windows gate passes. |",
        "| M23 — V0.0.3 integration proof | **Partial — acceptance running** | `OrbitV003ScaffoldIntegrationTests` now generates one real project, attaches the production analytic terrain stack to its rocky body through `SurfaceRegistry`, and exercises save/reopen, an additional moving body/frame, an external Luau plugin, JSON-RPC/MCP-equivalent editing, PBR/decal authoring with undo/redo, Direct/Bézier/Routed paths, Steam mapping validation, actual `OrbitPlayer` packaging, and deterministic rebuild after deleting DDC. Studio also owns two independently resizable GPU RenderViews. Final status closes only after the Windows gate passes. |",
        "M23 status terrain wording",
    )
    spec = replace_once(
        spec,
        "- it proves a primary rocky body plus an independently moving child-body frame;\n",
        "- it attaches `AnalyticTerrainSource` to the primary rocky body through the permanent `SurfaceRegistry` terrain capability and samples it;\n- it proves an independently moving child-body frame alongside that terrain-capable body;\n",
        "M23 proof terrain bullet",
    )
    SPEC.write_text(spec, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
