import finalize_v003_scaffold as finalizer


def patch_build_cmake() -> None:
    path = "engine/build/CMakeLists.txt"
    text = finalizer.read(path)
    marker = "        Orbit::PluginManifest\n"
    text = finalizer.replace_once(
        text,
        marker,
        marker + "        Orbit::PlatformServices\n",
        "Build CMake platform services link",
    )
    finalizer.write(path, text)


def patch_spec() -> None:
    # Reuse the authored M21/M22 checkpoint update, then reconcile milestones
    # whose implementation/acceptance coverage has landed since that helper
    # was written. "Implemented" in this table means production architecture
    # is in-tree; CI/visual acceptance remains separately stated.
    original_patch_spec()
    path = "docs/V0.0.3_SPEC.md"
    text = finalizer.read(path)

    text = text.replace(
        "| M18 — Path network core | Partial |",
        "| M18 — Path network core | Implemented |",
        1,
    )
    text = text.replace(
        "| M19 — Routed/dynamic path solver | **Partial** |",
        "| M19 — Routed/dynamic path solver | **Implemented** |",
        1,
    )
    text = text.replace(
        "| M20 — Path-derived geometry and AI graph | **Partial** |",
        "| M20 — Path-derived geometry and AI graph | **Implemented** |",
        1,
    )

    text = text.replace(
        "[x] clean-machine Windows CI rebuild path; final V0.0.3 cross-system example proof remains M23.",
        "[ ] clean-machine Windows CI rebuild acceptance; final V0.0.3 cross-system example proof remains M23.",
        1,
    )

    # Keep acceptance wording explicit while the replacement Windows run is
    # still the gate for closing the release, rather than implying a pass from
    # implementation alone.
    text = text.replace(
        "Status: **active implementation — M22 implemented; integration proof in progress**",
        "Status: **active implementation — M22 implementation in-tree; acceptance and M23 integration proof in progress**",
        1,
    )

    finalizer.write(path, text)


original_patch_spec = finalizer.patch_spec
finalizer.patch_build_cmake = patch_build_cmake
finalizer.patch_spec = patch_spec

if __name__ == "__main__":
    finalizer.main()
