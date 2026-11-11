+++
path = "/rendering/atmosphere/authoring-solver"
title = "Atmosphere authoring solver and provenance"
kind = "concept"
status = "stable"
owner_module = "OrbitWorldModel"
summary = """
AtmospherePropertySolver derives the M21 runtime coefficients from simple inputs (pressure, temperature, gravity, N2/O2/Ar/CO2 \
fractions, aerosol optical depth/albedo/Angstrom/scale height, absorber scale) when the author chooses Derived Composition, and \
never overwrites Explicit, Imported or Locked values: those are reported as conflicts. It writes through CommandService with \
provenance, so edits undo atomically and change the ordinary AtmosphereFingerprint."""
keywords = ["solver", "authoring", "provenance", "preset", "earth-like", "derived composition", "expert coefficients", "pressure", "composition", "aerosol", "locked", "explicit", "conflict", "scale height"]
sources = [
  "engine/world_model/include/orbit/world_model/AtmospherePropertySolver.hpp",
  "engine/world_model/src/AtmospherePropertySolver.cpp",
  "engine/world_model/include/orbit/world_model/PropertyProvenance.hpp",
  "engine/world_model/include/orbit/world_model/PropertyProvenanceStore.hpp",
  "engine/studio_ui/src/CelestialAuthoringUi.cpp",
  "engine/editor_model/src/CelestialRecipeService.cpp",
  "engine/world_model/tests/AtmospherePropertySolverTests.cpp",
  "engine/editor_rpc/src/EditorRpcService.cpp",
  "engine/editor_rpc/tests/EditorRpcServiceTests.cpp",
]
symbols = ["AtmospherePropertySolver", "AtmosphereReportToValue", "AtmosphereSolveReport", "AtmosphereSolveOutcome", "PropertyProvenance", "PropertySourceMode", "PropertySolveState", "PropertyProvenanceStore"]
invariants = [
  "There is still one persistent Atmosphere capability and one set of runtime coefficients; the solver writes the same physical properties ResolveAtmosphereBody reads, so any successful solve changes the ordinary AtmosphereFingerprint. No parallel representation or solver cache exists.",
  "Write policy per predicted coefficient: Default/Derived/Procedural writable authority may be solved; Explicit, Imported and Locked authority is preserved and any disagreement is returned as AtmosphereSolveOutcome::Conflict, never silently overwritten.",
  "A stored property with no provenance metadata is treated as Explicit + Locked, so legacy and manual expert values are never overwritten.",
  "Derived writes get SourceMode = Derived, SolveState = Solved and an explanation string of the derivation.",
  "Expert Coefficients mode performs no derivation; Derived Composition runs the solver; both leave the shared Inspector as the exact numeric editor and raw coefficients under Advanced Properties.",
  "Mean molar mass uses normalised N2/O2/Ar/CO2 fractions (0.0280134, 0.0319988, 0.039948, 0.0440095 kg/mol); scale height H_R = R_gas T / (Mbar g); gravity g = G M / R^2 only when mass and radius exist and gravity authority is writable.",
  "Aerosol: AOD(lambda) = AOD_550 (lambda/550 nm)^(-Angstrom) at 680/550/440 nm; beta_M_ext = AOD / H_M; beta_M_scat = beta_M_ext x single-scattering albedo; derived anisotropy g = 0.8.",
  "Absorber layer: centre = 3.125 H_R, half width = 1.875 H_R (reproduces the 25 km / 15 km Earth-like baseline at H_R about 8 km); the absorber coefficients scale with the absorber column scale.",
  "Atmosphere top radius (when writable) = bodyRadius + max(10 H_R, 12 H_M, absorberCenter + 2 absorberHalfWidth, 1 m); recipe-authored explicit extents stay authoritative.",
  "A manual Inspector edit of a property that has a provenance record promotes it to Explicit/Locked in the same transaction; undo restores value and provenance atomically. Provenance lives as a child record under the owning capability and is removed atomically with it.",
  "Every solver operation reachable from the Celestial panel (Solve, presets) is also an RPC method and MCP tool (atmosphere.solve / atmosphere.apply_preset / atmosphere.presets); keep them calling AtmospherePropertySolver, never duplicating its logic.",
  "Presets (Earth-like, Thin CO2, Dense CO2, Dry Nitrogen) write ordinary semantic properties with Procedural/Solved provenance and immediately run the same solver; no preset creates a hidden runtime type.",
]
related = ["/rendering/atmosphere/lut-pipeline", "/editor/mcp-rpc"]
depends_on = ["/rendering/atmosphere"]
verify = [
  "ctest -R Orbit.AtmospherePropertySolver: Earth-like derived scale height, provenance persistence, expert value preservation, imported/locked preservation, conflict reporting, one-transaction preset + solve undo/redo, pressure edit invalidating the fingerprint, Expert mode performing no derivation, Inspector edit promotion.",
  "ctest -R Orbit.PropertyProvenance and Orbit.CelestialAtmosphereBinding.",
]
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"

[[diagnose]]
symptom = "derived atmosphere values did not change after editing pressure or composition"
steps = [
  "Check the authoring mode: Expert Coefficients performs no derivation; switch to Derived Composition and run Solve Derived Coefficients.",
  "Read the AtmosphereSolveReport events: a Conflict means the target property is Explicit, Imported or Locked; unlock or clear that property if the solver should own it.",
  "A property with no provenance record counts as Explicit + Locked; give it provenance (or reset it) before expecting the solver to write it.",
]
docs = ["/rendering/atmosphere/lut-pipeline"]
+++

## RPC / MCP

The solver is reachable without the UI (MCP parity, `/editor/mcp-rpc`). Registered in `EditorRpcService.cpp` next to the celestial
methods; each calls the same `AtmospherePropertySolver` the Celestial panel buttons call:

| RPC method | MCP tool | Effect |
|---|---|---|
| `atmosphere.presets` | `orbit_atmosphere_presets` | Lists preset names (Earth-like, Thin CO2, Dense CO2, Dry Nitrogen). |
| `atmosphere.solve` | `orbit_atmosphere_solve(atmosphere_id)` | `Solve`: derives writable coefficients from the authored inputs. |
| `atmosphere.apply_preset` | `orbit_atmosphere_apply_preset(atmosphere_id, preset)` | `ApplyPreset`: one undoable transaction, then the same solve. |

`atmosphere_id` is the atmosphere **capability** object (a child of the body), not the body id. Both solver methods return
`{events: [{property, name, outcome, explanation}], derived_count, has_conflict, has_invalid_input}` with outcome one of
`derived`, `no_change`, `conflict`, `invalid_input`. A conflict is a normal result, not an RPC error: it
means a Locked/Explicit/Imported property was preserved. A non-atmosphere object or an unknown preset is a `-32602` error and
changes nothing. Tests: `engine/editor_rpc/tests/EditorRpcServiceTests.cpp` (preset conflict on a locked top radius, wrong
object type, unknown preset, idempotent re-solve).

## Intentional limits (stated at the M22 baseline)

No full chemical equilibrium, condensation or cloud microphysics, line-by-line spectroscopy, arbitrary molecular species,
pressure/temperature vertical profile tables or imported atmospheric profile formats. These extend the authoring layer
while still resolving into the same runtime parameters. Source: `/legacy/tree-history-research-v006-atmosphere-authoring-solver`.
