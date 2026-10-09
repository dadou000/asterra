# Stellar habitable-zone transport indicator

- Existing owner: `SimulationControlsUi` owns the transport band and its daytime scrubber presentation; `CelestialRadiometryBinding` resolves authored emitter output.
- Primary insertion point: add a read-only star-system diagram branch to the existing scrubber canvas when the active celestial body has a valid radiative emitter. Keep the planet day/night scrubber and time mutation path as-is.
- Canonical state: authored luminosity, stellar photosphere parameters and sibling orbit capabilities remain in the world `ObjectStore`; the shared simulation clock remains in `SimulationControls`.
- Do not add another orbit model or mutate simulation state from the diagram. Plot siblings' semi-major axes and calculate a guide band from irradiance thresholds.
