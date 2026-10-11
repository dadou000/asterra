+++
path = "/rendering/unified-transport/storm-electrification"
title = "Storm electrification and lightning (CPU reference)"
kind = "subsystem"
status = "experimental"
summary = """
OrbitStormElectrification builds charge from graupel-ice collisions, carries it vertically, solves the electric field, and grows stochastic bidirectional-leader \
flashes (intracloud, cloud-to-cloud, cloud-to-ground) that neutralise charge and emit a time-resolved DischargeGraph; a random-access flash schedule answers timewarp. \
It is a std-only CPU reference with uncalibrated magnitudes; no weather solver, renderer or RPC uses it yet."""
owner_module = "OrbitStormElectrification"
keywords = ["lightning", "electrification", "charge", "graupel", "poisson", "leader", "discharge", "flash", "return stroke", "timewarp", "breakdown", "field"]
sources = [
  "engine/storm_electrification/include/orbit/storm_electrification/ChargeGrid.hpp",
  "engine/storm_electrification/include/orbit/storm_electrification/Electrification.hpp",
  "engine/storm_electrification/include/orbit/storm_electrification/LeaderSimulation.hpp",
  "engine/storm_electrification/include/orbit/storm_electrification/FlashSchedule.hpp",
  "engine/storm_electrification/include/orbit/storm_electrification/WarmRainAdapter.hpp",
  "engine/storm_electrification/include/orbit/storm_electrification/StormLightningSource.hpp",
  "engine/storm_electrification/CMakeLists.txt",
]
symbols = ["ChargeGrid", "StepElectrification", "TryFlash", "FlashSchedule", "MaxBreakdownRatio"]
invariants = [
  "ChargeGrid::SolvePotential solves -div(eps0 grad phi) = rho by conjugate gradients with 0 V on every face of the block; the grounded planes sit half a cell outside the first and last cell centres. A charged slab between grounded walls matches the analytic parabola to 1 %.",
  "Charge lives on two carriers (ice, lofted; graupel, falling). StepElectrification charging is neutral by construction (graupel gains what ice loses) and vertical upwind transport is conservative: total charge + charge out through the top + charge delivered to the ground is constant.",
  "Graupel charges negative warmer than reversalTemperatureK (258.15 K) and positive colder, smoothly over reversalWidthK; no charging above freezing or without supercooled liquid. All magnitudes (chargingEfficiency, thresholds, energies) are order-of-magnitude defaults, not fitted.",
  "The FastStormSolver has no ice phase, so StormFields.graupel / ice / liquid must be supplied by the caller (for example derived from supercooled cloud and rain above the freezing level). Charge is transported vertically only. WarmRainAdapter.hpp (StormFieldsFromWarmRain) is that caller-side proxy: rain colder than 268 K is half graupel and half snow, cloud water colder than 253 K is ice, between that and 0 C it is supercooled liquid. It places the charging regions; it does not set their strength. StormLightningSource (StormLightningSource.hpp) steps a frozen field snapshot at timeScale simulated seconds per wall second (default 20) and returns the FlashResults, in grid coordinates, for a host to place and give to TransientFlashRegistry::Add; granularity-independent, about 25 ms per 5 s step on 40x40x28, so a worker thread at 20x costs roughly a tenth of a core. Advance(wall, maxSteps) bounds one call's work and drops the surplus, so a slow machine runs the storm slower than asked instead of ever further behind (an unbounded dt fed back into a long Advance made the first controller hang). Studio wiring: studio_ui StormLightningController (RPC lightning.storm_start) runs it on a worker thread against a WeatherLabSession::LiveSnapshot (ctest -R Orbit.StormLightningSource, Orbit.StormLightningController).",
  "TryFlash starts at the cell with the highest |E| / (initiationField * exp(-z / scaleHeight)) when that is >= 1; the channel is an equipotential that grows into neighbours whose potential differs by more than propagationField * density factor per cell, with probability proportional to the excess. Connecting to the ground pins the channel at 0 V.",
  "A flash stops growing when the neutralisable charge near the channel reaches maxFlashCharge (once minChannelCells exist), when growth stagnates, or at maxChannelCells; it never neutralises more than maxFlashCharge of either polarity. An ungrounded flash removes exactly equal positive and negative charge; a grounded flash delivers the net to the ground (FlashResult::groundCharge) and the grid's total changes by exactly that.",
  "A flash is only accepted if it lowers the field energy: the neutralisation is tried at full strength and halved (from a snapshot of the charge) up to three times; if none lowers the energy the grid is restored and no flash happens. Without this, 1 in 61 large cloud-to-cloud flashes slightly raised the energy because the second-order term of moving ~25 C over a large channel beat the first-order dissipation.",
  "Every flash lowers the field energy; the same grid, seed and flash index reproduce the same flash bit for bit; a different seed grows a different channel.",
  "The DischargeGraph of a cloud-to-ground flash carries a faint leader pulse on every segment and a return-stroke wave up the grounded path at returnStrokeSpeed radiating returnStrokeEnergy; intracloud and cloud-to-cloud flashes have no return stroke. EnergyInWindow integrates microsecond pulses fully into a frame-length exposure and ClusterEmission preserves the energy within a bounded cluster count.",
  "FlashSchedule is an inhomogeneous Poisson process evaluated by unit-intensity strata hashed from (seed, stratum): any window gives the same flashes however it is split, a window a year ahead costs only its own strata, and the mean count over seeds equals the integrated rate.",
]
related = ["/rendering/unified-transport", "/rendering/unified-transport/core", "/rendering/unified-transport/design-review"]
depends_on = ["/rendering/unified-transport/core"]
verify = [
  "g++ -std=c++23 -O2 -Iengine/storm_electrification/include -Iengine/optical_transport/include engine/storm_electrification/src/*.cpp engine/optical_transport/src/DischargeGraph.cpp engine/storm_electrification/tests/StormElectrificationTests.cpp (about 25 s); the repo CMake targets Windows only: ctest -R Orbit.StormElectrification there.",
]
verified = "052ee04"

[[diagnose]]
symptom = "a strongly charged storm never flashes (field ratio keeps rising)"
steps = [
  "TryFlash returns occurred = false when the channel could not grow past three cells. Check the charge budget logic first: the neighbourhood of one cell can hold more than maxFlashCharge, which is why minChannelCells exists.",
  "Print MaxBreakdownRatio's cell: a maximum on the block boundary is a boundary artefact (the ghost cells are 0 V); pad the block so the storm is far from every face.",
]
docs = ["/rendering/unified-transport/design-review"]
+++

## What it does

Per step: `StepElectrification` creates charge separation where graupel meets ice in supercooled liquid below freezing, then moves both carriers up and down with the
air and their fall speeds. `ChargeGrid::SolvePotential` gives the field; when `MaxBreakdownRatio >= 1`, `TryFlash` grows a channel, neutralises charge near it and returns a
`FlashResult` whose `graph` is a time-stamped `optical_transport::DischargeGraph` ready to be turned into bounded emission clusters for the lighting. `FlashSchedule` answers
"which flashes happen in this window" at absolute time without simulating the interval, for extreme timewarp.

## What the tests show

- With supercooled liquid limited to warmer than about -20 C (colder, an updraft is glaciated and has no riming charge), the default synthetic storm builds a normal dipole: 23 C each, centroids 2.5 km apart, main negative at about 265 K (-8 C), positive above it. Letting liquid reach -28 C instead puts negative ice aloft; that was a fixture error, not a model error.
- First breakdown after about 6 minutes of charging at 25 m/s; flash count in 15 minutes at 8 / 18 / 30 m/s peak updraft (condensate scaled with the updraft): 0 / 15 / 62. Stronger storms flash more, superlinearly.
- A vigorous storm separates about 2 C per second and each flash neutralises about 25 C (the configured budget), giving several flashes per minute: the right order of magnitude, not a calibration.
- A synthetic low positive pocket under a negative layer produces a cloud-to-ground flash that lowers about 5 C of negative charge to the ground.

## Known gaps

- There is no lower positive charge pocket, so the storm is a dipole, not a tripole; a lower pocket (graupel charging positive at low liquid water, or melting-level effects) is not modelled.
- Charge moves vertically only. A real FastStormSolver storm (64x64x40 at 500 m) driven through the ice proxy does electrify once mature (no flash at 10 min; ratio 1.03 and 8 flashes in 900 s at 20 min, 94 at 25 min, ratio 15 at 35 min, field snapshots held frozen), but the ice phase is assumed and the strength is untuned (ctest -R Orbit.WarmRainAdapter, ~55 s). The channel potential is fixed rather than re-solved as it grows.
- No leader branching physics beyond the stochastic growth; sprites, megaflashes and superbolts are not modelled. Nothing consumes the flash graph in a renderer: the transient-lighting injection
  (bounded GPU cost, higher-order scattering response) is not built.
- No RPC/MCP exposure, which `AGENTS.md` requires before this is reachable from Studio.
