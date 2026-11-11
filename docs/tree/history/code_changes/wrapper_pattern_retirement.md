# Retiring the include/macro wrapper pattern

## Behavior
No behaviour change. Eight translation units were built by `#define`-renaming the original method names and `#include`-ing the original `.cpp` (or `.inc`) into a wrapper that adds behaviour and calls the renamed originals. Each pair is now one ordinary source file where the original methods are simply named `...Base`, exactly what the macros produced.

| Unit | Original (included) | Renamed methods |
| --- | --- | --- |
| `lighting/LightingScheduler.cpp` | `LightingSchedulerBase.cpp` | `RecordGpuTimingsBase`, `BuildPlanBase` |
| `studio_ui/DisplayDiagnosticsUi.cpp` | `DisplayDiagnosticsUiBase.cpp` | `RegisterBase`, `DrawViewportBase` |
| `studio_ui/ProjectSettingsUi.cpp` | `ProjectSettingsUiBase.cpp` | `RegisterBase`, `DrawBase` |
| `studio_ui/StudioViewportPanels.cpp` | `StudioViewportPanelsBase.cpp` | `RegisterBase`, `RegisterSecondaryBase`, `DrawViewBase` |
| `studio_ui/StudioViewportRenderer.cpp` | `StudioViewportRendererBase.cpp` (15,000 lines) | `ComposeBase` |
| `studio_ui/VolumeAuthoringUi.cpp` | the old `VolumeAuthoringUi.cpp` + `VolumeAuthoringM36.cpp` | `RegisterBase`, `DrawBase` |
| `volume_render/UniversalVolumeRenderer.cpp` | `UniversalVolumeRendererBase.inc` | `AddLivePasses`, `LiveDiagnostics` |
| `volume_solver`, `volume_fields` | `SurfaceVolumeSolverBase.inc`, `VolumeFieldStorageBase.inc` | `AddPassesBase`, `EnsureBase` |

`celestial_globe/MacroGlobe.cpp` was renamed through CMake compile definitions in the same way; the symbols are now spelled in the file.

## Existing owner
- Module: each unit's own module; public classes and headers keep their declarations (`Foo` and `FooBase` were both declared already).
- Canonical state: unchanged.

## Primary insertion point
- File: the eight units above and the headers that guarded declarations with `ORBIT_*_BASE_IMPLEMENTATION` / `ORBIT_BUILD_LEGACY_MACRO_GLOBE_RENDERER`
- Symbol/function: the renamed method definitions; the merge applies the macro renames with a C++-aware lexer (strings, comments and raw strings untouched; `push_macro`/`undef` regions honoured) and the diff of the base text is only the renamed definitions and calls.
- Reason: a `.cpp` that includes another `.cpp` is invisible to tooling, doubles the mental model of every edit and cannot be split later.

## Secondary touch points
- `engine/*/CMakeLists.txt` (`VolumeAuthoringM36.cpp` -> `VolumeAuthoringUi.cpp`, the globe's `set_source_files_properties` removed), docs blocks that listed the `Base` files or described the macro trick.

## Must not be implemented in
- A new wrapper layer: behaviour added on top of a `...Base` method goes into the public method that calls it.

## Data/control flow
`Foo(...)` (new behaviour) -> `FooBase(...)` (original implementation), as before

## Validation
- [x] Same translation-unit contents as the preprocessor produced (only definitions/calls renamed)
- [x] Full rebuild, 260/260 ctest tests pass, Studio smoke passes
