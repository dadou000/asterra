from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


def patch_tests() -> None:
    path = ROOT / "tests/CMakeLists.txt"
    text = path.read_text(encoding="utf-8")
    marker = "OrbitV003ScaffoldIntegrationTests"
    if marker in text:
        return

    text += r'''

add_executable(OrbitV003ScaffoldIntegrationTests
    V003ScaffoldIntegrationTests.cpp
)

target_link_libraries(OrbitV003ScaffoldIntegrationTests
    PRIVATE
        Orbit::Build
        Orbit::Content
        Orbit::ContentWic
        Orbit::EditorModel
        Orbit::EditorRpc
        Orbit::Frames
        Orbit::Paths
        Orbit::PlatformServices
        Orbit::Plugins
        Orbit::Universe
)

target_compile_features(OrbitV003ScaffoldIntegrationTests PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitV003ScaffoldIntegrationTests)
add_dependencies(OrbitV003ScaffoldIntegrationTests OrbitPlayer)
orbit_copy_dxc_runtime(OrbitV003ScaffoldIntegrationTests)

add_test(
    NAME Orbit.V003ScaffoldIntegration
    COMMAND OrbitV003ScaffoldIntegrationTests $<TARGET_FILE:OrbitPlayer>
)
'''
    path.write_text(text, encoding="utf-8", newline="\n")


def patch_spec() -> None:
    path = ROOT / "docs/V0.0.3_SPEC.md"
    text = path.read_text(encoding="utf-8")

    old_m17 = "| M17 — Material Service | Partial | Dockable Material Service, indexing/search, PBR-set import, reusable material instances, material drag payloads, schema-backed assignment and transactional viewport drop are implemented. Decal workflow and rendered material preview remain. |"
    new_m17 = "| M17 — Material Service | **Implemented** | Dockable Material Service now includes indexed PBR materials/instances, dependency-tracked `.orbitdecal` assets, texture-to-decal authoring, transactional body-surface decal drops, undoable semantic decal objects, and a second resizable Vulkan `RenderView` with real roughness/metallic lighting and imported base-color sampling. |"
    text = replace_once(text, old_m17, new_m17, "M17 status")

    old_m23 = "| M23 — V0.0.3 integration proof | **Partial** | The permanent scaffold is now wired end-to-end through Studio, CLI, runtime, RPC/MCP, project documents, paths, materials/plugins and platform services. Final example-project proof and remaining visual/editor acceptance cases are tracked here rather than represented by placeholder systems. |"
    new_m23 = "| M23 — V0.0.3 integration proof | **Partial — acceptance running** | `OrbitV003ScaffoldIntegrationTests` now generates one real project and exercises save/reopen, two moving celestial frames, an external Luau plugin, JSON-RPC/MCP-equivalent editing, PBR/decal authoring with undo/redo, Direct/Bézier/Routed paths, Steam mapping validation, actual `OrbitPlayer` packaging, and deterministic rebuild after deleting DDC. Studio also owns two independently resizable GPU RenderViews. Final status closes only after the Windows gate passes. |"
    text = replace_once(text, old_m23, new_m23, "M23 status")

    m17_acceptance = '''Acceptance:

- importing a PBR set creates a reusable material asset;
- dragging it onto a compatible object assigns it transactionally;
- undo restores the previous assignment.

### M18 — Path network core'''
    m17_checkpoint = '''Current implementation checkpoint:

- [x] indexed/searchable Material Service panel and thumbnail identities;
- [x] PBR-set import and reusable material instances;
- [x] material drag/drop assignment through the shared command/transaction path;
- [x] dependency-tracked `.orbitdecal` authority assets;
- [x] texture-to-decal creation and body-surface drop placement using explicit surface coordinates;
- [x] persistent editable Surface Decal schema with undo/redo;
- [x] independently resizable Vulkan material preview RenderView;
- [x] preview uses imported base-color texture data when available plus material roughness/metallic factors.

Acceptance:

- importing a PBR set creates a reusable material asset;
- dragging it onto a compatible object assigns it transactionally;
- undo restores the previous assignment.

### M18 — Path network core'''
    text = replace_once(text, m17_acceptance, m17_checkpoint, "M17 checkpoint")

    proof_marker = '''V0.0.3 is complete only when this project can be reopened and rebuilt from authoritative
source data after deleting all derived caches.
'''
    proof_new = proof_marker + '''
Current proof implementation:

- `OrbitV003ScaffoldIntegrationTests` creates the proof project from authoritative source data;
- it persists and reopens the world document;
- it proves a primary rocky body plus an independently moving child-body frame;
- it loads a real external Luau editor plugin and panel/command registration;
- it performs a structured JSON-RPC edit through `EditorRpcService`, the engine endpoint used by MCP;
- it assigns a PBR material and creates/undoes/redoes a semantic surface decal through shared commands;
- it creates Direct, Bézier and Routed semantic path edges;
- it validates Steam achievement/stat/timeline mappings without requiring Steam for standalone execution;
- it packages the real `OrbitPlayer.exe`, deletes DDC, packages again, and requires identical build manifests;
- Studio's body viewport and Material Service preview are separate resizable `RenderView` instances.
'''
    text = replace_once(text, proof_marker, proof_new, "M23 proof checkpoint")

    path.write_text(text, encoding="utf-8", newline="\n")


def main() -> None:
    patch_tests()
    patch_spec()


if __name__ == "__main__":
    main()
