# Continuous plate boundary masks

- Existing owner: `TectonicField::Sample` (CPU) and
  `SampleTectonicConvergenceAndBias` in `FieldGenerationCompute.hpp` (GPU copy).
- Problem: boundary strength, normal and relative velocity were computed against
  only the nearest and runner-up plate, so convergence jumped (up to 1.0 across
  0.25 degree) along the line where the runner-up changed. Mountain belts ended
  in straight cuts that started at triple junctions (~99% of 836 jumps in a
  sweep of one seed).
- Change: all pairs among plates within one boundary width of the top are
  evaluated and max-combined; the plate bias, structural crust thickness and age
  use symmetric g/(1-g) weights; per-collision-class convergence masks replace
  the runner-up plate's continental flag in macro geology and the structure
  layer. The GPU function mirrors the CPU one.
- Canonical state: none added. Terrain shape changes slightly near junctions;
  worlds regenerate.
- Tests: `BoundaryMasksAreContinuous` in `tests/TectonicStructureTests.cpp`
  (4 seeds, 0.25 degree sweep), plus the existing CPU/GPU field parity and
  mountain survey tests.
