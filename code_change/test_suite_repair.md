# Test suite repair (0.0.9 cleanup)

## Behavior
No engine behaviour change except one bug fix. Before this pass 17 ctest tests failed and 7 test files did not compile (all identical at the base commit, none caused by the cleanup); now the full suite builds with warnings as errors and all 260 non-skipped ctest tests pass.

Production bug found and fixed: `SaveLightingDisplaySettings` (`LightingDisplay.orbitcfg`, the project lighting/display defaults) renamed its temporary file while the `ofstream` was still open. Windows refuses that, so every save threw "Failed to replace LightingDisplay.orbitcfg". The stream is now closed before the rename, like the other two save paths.

Other findings: eleven tests used `assert()`, which `NDEBUG` removes in Release, so they checked nothing; they now `#undef NDEBUG`, and two of them (`BodyRegistry`, `RenderGraph`) were hiding stale expectations. `Orbit.JobSystem` raced itself (`Wait()` runs queued jobs on the calling thread).

## Existing owner
- Module: each test belongs to the module it tests.
- Canonical state: production behaviour is the reference; stale tests were changed to the documented behaviour, never the other way round.

## Primary insertion point
- File: the test sources (see the commit `Fix the 17 failing and 7 non-compiling tests`), `engine/studio_ui/include/orbit/studio_ui/LightingDisplaySettings.hpp` for the one production fix, `tests/JobSystemTests.cpp`
- Reason: each failure was traced to its cause before editing; for example quarter-orbit positions are compared with a tolerance relative to the orbit radius because the quarter instant is truncated to whole microseconds.

## Secondary touch points
- The five new notes and the static-mesh doc are linked into the docs tree so the docs tooling tests pass.

## Must not be implemented in
- Production code written to satisfy a stale expectation.

## Data/control flow
failing test -> cause (stale expectation, test race, real bug) -> fix at the cause

## Validation
- [x] `ctest -C Release` 260/260, `Orbit.JobSystem` stable over 1,500 repeats
- [x] `python -m unittest discover -s tools/tests -p test_orbit_docs.py` and `python tools/orbit_docs_cli.py check` pass
