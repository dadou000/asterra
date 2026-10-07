# Orbit V0.0.6 — Physical Property Solver v1 Research Note

Status: M03 implementation baseline.

## Scope

The first solver is intentionally narrow and deterministic. It establishes the dependency/provenance contract used by later celestial physics rather than attempting to model every astronomical relation in one pass.

All authoritative quantities are SI internally.

## Implemented relations

### Mean spherical density

For a spherical reference radius:

```
V = (4/3) pi r^3
rho = m / V
m = rho V
r = cbrt(m / ((4/3) pi rho))
```

This is a mean-density relation. Oblate/irregular volume models will replace the spherical volume term where their shape capability requires it.

### Equatorial rotation speed

```
v_eq = 2 pi r / P_rot
```

This is geometric surface speed at the reference equator. It does not include atmospheric motion or differential rotation.

### Two-body orbital period

```
P = 2 pi sqrt(a^3 / mu)
```

where `a` is semi-major axis and `mu` is the gravitational parameter of the selected two-body authority. M05 will own the full conic-state model.

### Stellar effective radius

Using Stefan–Boltzmann luminosity:

```
L = 4 pi R^2 sigma T_eff^4
R = sqrt(L / (4 pi sigma T_eff^4))
```

Orbit uses the exact SI Stefan–Boltzmann constant adopted by the solver:

```
sigma = 5.670374419e-8 W m^-2 K^-4
```

This derives an effective radiating radius. It is not a stellar-structure model.

## Solver authority rules

The M02 provenance contract is authoritative.

A solver may write only when `CanSolverWrite()` permits it. Explicit, imported, locked and conflicting values are never silently replaced. If an authoritative value disagrees with a predicted physical constraint, the solver emits a conflict event and leaves the value unchanged.

Each derived event records:

- target property;
- dependency property names;
- outcome;
- the equation/reason used.

This event stream is the v1 dependency-explanation surface used by diagnostics and future Inspector UI.

## Determinism and validation

The implementation uses double-precision SI arithmetic and no random state.

Reference tests cover:

- Earth-like mean density;
- Earth sidereal equatorial speed;
- 1 AU period around the solar gravitational parameter;
- solar effective radius from luminosity and effective temperature;
- explicit-value conflict preservation;
- invalid non-positive physical inputs.

## Intentional limits

M03 does not yet solve:

- ellipsoid or irregular-body volume;
- uncertainty propagation;
- multi-body orbital period corrections;
- general stellar structure;
- atmospheric thermodynamics;
- iterative over-constrained least-squares systems.

Those extend the same provenance/dependency contract rather than replacing it.
