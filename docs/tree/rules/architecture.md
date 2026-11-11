+++
path = "/rules/architecture"
title = "Architecture rules"
kind = "rule"
status = "stable"
summary = """
Dependencies point one way, there is no global Engine object or service locator, modules own \
their types, world state is never render state, and generated geometry is a disposable cache. \
Violations are bugs: redesign the boundary instead of patching around it."""
keywords = ["architecture", "dependency", "module", "ownership", "core", "god object", "service locator", "layering"]
sources = ["docs/ORBIT_ARCHITECTURE.md", "CMakeLists.txt"]
applies_to = ["engine/**", "apps/**"]
invariants = [
  "Dependencies are one-way: apps -> high-level systems -> world/physics/render/assets -> core. A lower layer never includes a higher one.",
  "No global Engine object and no GetRenderer()/GetWorld() accessors; systems receive narrow dependencies through constructors or context objects.",
  "A type has exactly one owning module; cross-module use goes through public headers, IDs, handles and plain data.",
  "The persistent world holds no GPU handles, descriptor indices, renderer pointers or draw objects (world -> simulation -> extraction -> GPU scene).",
  "The persistent planet is compact authoritative data, not the live ECS; only relevant regions are promoted to runtime objects.",
  "OrbitCore holds only broadly reusable primitives; terrain, vehicle, rendering or gameplay helpers do not go there.",
  "Procedural: semantic blueprint is authoritative; generated mesh, collision, navigation and HLOD are caches.",
  "Solvers (rigid body, CFD, water, weather, GPU fields) communicate through small explicit interfaces, never by reaching into each other's state.",
  "Engine APIs do not silently require the main thread; expensive work must be expressible as jobs.",
  "The editor is a privileged client of engine APIs, not a parallel implementation of the world.",
]
related = ["/rules/placement", "/rules/hot-iteration"]
+++

The authoritative text is `docs/ORBIT_ARCHITECTURE.md` (19 numbered rules). This node keeps the
parts agents violate most often.

## Quick checks before you add an include or a type

- Does the new `#include` point **up** the layer stack? Then the design is wrong; invert it with an
  interface, an ID or a data transfer object.
- Are you adding a global accessor or a manager that "knows everything"? Pass what is needed instead.
- Is the type already owned by another module? Extend that module (see `/rules/placement`).
- Is a third-party library (Jolt, DLSS, DirectX libs, audio middleware) leaking beyond its adapter?
- Is generated data being treated as the source of truth? Keep the semantic data authoritative.

## Module layout convention

```text
engine/<module>/
  include/orbit/<module>/   public headers (minimum surface, forward-declare where possible)
  src/                      implementation
  tests/                    module tests (registered with add_test)
  docs/                     documentation blocks attached to the docs tree
```

Keep translation units and public headers narrow: one-file edits should stay cheap
(this is also what makes hot iteration fast).
