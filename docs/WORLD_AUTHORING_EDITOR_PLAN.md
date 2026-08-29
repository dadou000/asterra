# Asterra World Authoring Editor — Implementation Plan

Status: design / implementation plan
Target engine: Godot 4.7.1 stable

## 1. Goal

Build a dedicated in-game world authoring environment for Asterra that can edit the active celestial body and its visual/runtime world definition without turning the existing gameplay `TerrainEditor` into a monolithic tool.

The editor should feel closer to Blender's property + node workflow than to a debug menu. It must support:

- categories: **Planet**, **Terrain**, **Water**, **Atmospheric**, **Celestials**;
- **Save Preset**, **Load Preset**, **Apply**, **Undo**, and **Redo**;
- active-body selection from a celestial map;
- creating, duplicating, switching to, and deleting celestial bodies;
- editing orbital and rotational parameters;
- ring systems;
- post-generation biome painting;
- authored lakes and Bézier rivers;
- a common clipmap-driven water runtime for ocean/lake/river surfaces;
- separate displacement and material slot stacks;
- clipmap-level and biome targeting per slot;
- multiple displacement/material graphs on the same clipmap level;
- signed displacement composition so overlapping graphs can constructively or destructively interfere;
- a typed Blender-like node editor with Asterra runtime/context inputs;
- imported texture assets with explicit color-space/normal/scale metadata;
- transactional application, versioned persistence, and last-known-good shader rollback;
- performance budgets compatible with Asterra's GTX 1050 minimum target.

This is a world-authoring subsystem, not a replacement for the gameplay excavation/fill editor.

---

## 2. Existing architecture and constraints

### 2.1 Keep gameplay terrain editing separate

`scripts/terrain/terrain_editor.gd` currently represents physical gameplay earthmoving: dig/fill/grade, sparse terrain deltas, loose material stock, and piles. It should stay focused on simulation/gameplay.

The new authoring system should be named separately, for example:

- runtime/UI name: **Planet Studio**
- code root: `WorldAuthoringEditor`

Authoring operations can reuse sparse spherical tile techniques from terrain deltas, but authoring data should be stored in named non-destructive layers rather than mixed with player excavation.

### 2.2 Current planet runtime is effectively single-body

The current `Planet` autoload, `Frames`, terrain context, terrain clipmap, water, scatter and main scene assume one active generated planet. Multi-body authoring therefore requires an active-body facade rather than immediately rewriting every terrain subsystem around arrays of planets.

Initial compatibility target:

1. A persistent `CelestialSystemDefinition` owns every star/planet/moon.
2. `ActiveCelestialBodyRuntime` selects one body.
3. Existing `Planet` remains the runtime facade for the selected terrestrial body during migration.
4. Switching bodies invalidates/rebinds all body-specific GPU caches and context generations.

This minimizes risk to the authoritative terrain stack.

### 2.3 Double precision is mandatory for authored planetary geometry

Asterra already keeps canonical planetary state in `Vec3D` and only converts to local float32 render coordinates through `Frames`.

River control points, lake boundaries, orbital data, and permanent editor gizmos must therefore **not** be stored as raw global `Vector3` positions. Use double/spherical canonical data and create local float32 proxies for editing/rendering.

### 2.4 No extra draw pass per shader slot

A slot is an authoring/composition concept, not a render pass. Ten terrain material slots must not become ten complete terrain draws.

The graph compiler must lower all enabled slots for a quality tier into one generated displacement function and one generated material function. This is especially important after the recent scatter performance issue.

---

## 3. Editor UX

Use a dedicated full-screen `Control` scene rather than extending the current debug menu.

Suggested layout:

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│ [Body: Asterra ▼]   *Modified*       Save  Load  Undo  Redo  Apply  Revert │
├──────────────┬───────────────────────────────────────────────┬───────────────┤
│ PLANET       │                                               │ Inspector     │
│ TERRAIN      │            category workspace                 │               │
│ WATER        │                                               │ selected item │
│ ATMOSPHERIC  │                                               │ properties    │
│ CELESTIALS   │                                               │               │
├──────────────┴───────────────────────────────────────────────┴───────────────┤
│ validation / compile / cache / performance status                            │
└──────────────────────────────────────────────────────────────────────────────┘
```

For Terrain shader editing, the centre workspace becomes a `GraphEdit` plus a separate slot stack. For Water it becomes the 3D world viewport plus a feature list/inspector. For Celestials it becomes the hierarchy + orbital map.

### Toolbar behavior

- **Save Preset**: persist the current staged authoring snapshot.
- **Load Preset**: load into staging; do not silently destroy the active world.
- **Undo / Redo**: session history.
- **Apply**: validate and transactionally promote staged changes into the active runtime.
- **Revert**: discard staged changes and restore the currently applied state.
- Show a dirty marker and the highest required apply scope: `HOT`, `GRAPH`, `TILES`, `CLIPMAP`, `FULL REBUILD`.

### Live preview versus Apply

Use preview only where it is safe:

- numeric shader uniforms: live preview;
- atmospheric colors/densities: live preview;
- water wave/current parameters: live preview;
- graph topology: compile a preview shader asynchronously/deferred and retain last-known-good on error;
- biome painting/water vector features: preview through authoring overlays;
- planet radius, generation topology, seed: staged only, marked **FULL REBUILD**.

`Apply` is the authoritative transaction boundary.

---

## 4. Persistent data model

Use versioned custom `Resource` models and stable UUID/string IDs. Never use saved `NodePath`s as persistent body/feature identities.

### 4.1 CelestialSystemDefinition

```text
CelestialSystemDefinition
  schema_version
  system_id
  display_name
  bodies: Array[CelestialBodyDefinition]
  active_body_id
  shared_asset_library
```

### 4.2 CelestialBodyDefinition

```text
CelestialBodyDefinition
  body_id
  display_name
  body_type: STAR | PLANET | MOON | DWARF | OTHER
  parent_body_id

  physical
    radius_m
    mass_kg or gravitational_parameter
    optional density_lock

  rotation
    sidereal_rotation_period_s
    axial_tilt_deg
    pole_orientation
    rotation_phase_at_epoch
    epoch_s

  orbit
    semi_major_axis_m
    eccentricity
    inclination_deg
    longitude_ascending_node_deg
    argument_periapsis_deg
    mean_anomaly_at_epoch_deg
    epoch_s

  ring_system
  planet_profile (when applicable)
  star_profile (when applicable)
```

Internally use standard Keplerian elements. The UI can expose the more intuitive periapsis/apoapsis pair and continuously convert:

```text
r_peri = a * (1 - e)
r_apo  = a * (1 + e)
a       = (r_apo + r_peri) / 2
e       = (r_apo - r_peri) / (r_apo + r_peri)
```

The UI must clearly distinguish:

- centre-to-centre periapsis/apoapsis distance;
- optional convenience altitude above the parent body's reference radius.

Rotation is stored as seconds but displays an **hours/day** estimate and supports direct hours entry.

Radius and mass/gravity remain independent. An optional density lock can derive one from the other, but changing radius must not silently imply Earth-like density.

### 4.3 PlanetAuthoringProfile

```text
PlanetAuthoringProfile
  generation_profile
  terrain_profile
  water_profile
  atmosphere_profile
  ring_profile
  visual_asset_refs
```

Keep generation values separate from authored visual/runtime overlays so a biome paint or material change does not alter the deterministic pristine generator.

### 4.4 Versioning and migration

Every saved top-level profile must contain `schema_version`.

Add migration functions from day one:

```text
v1 -> v2 -> v3 ...
```

Do not make future field renames require manually repairing old planets.

---

## 5. Planet category

### 5.1 Physical

- name;
- type;
- radius;
- mass;
- surface gravity (derived/read-only unless user chooses a gravity-lock mode);
- gravitational parameter if physics model uses it;
- reference sea level;
- optional flattening/oblateness later.

### 5.2 Rotation

- sidereal rotation period;
- display as hours per day;
- axial tilt;
- rotation direction;
- phase at epoch;
- optional **tidally lock to parent** action.

### 5.3 Orbit

- parent/orbited object;
- periapsis;
- apoapsis;
- semi-major axis;
- eccentricity;
- inclination;
- longitude of ascending node;
- argument of periapsis;
- mean anomaly / orbital phase at epoch;
- epoch;
- derived orbital period;
- orbit validity warning if it intersects parent/rings/another required exclusion region.

For normal UI use, periapsis/apoapsis can be primary and the more technical elements can live under an **Advanced orbit** foldout.

### 5.4 Generation properties

Expose the existing `GenConfig` values in grouped sections, but annotate cost/scope:

- world seed;
- macro face resolution;
- tectonics/plate count;
- ocean fraction / shelf / abyss;
- erosion;
- climate generation;
- terrain detail generation;
- streaming settings.

Any property that participates in generation/cache identity is a rebuild property, not a harmless visual slider.

### 5.5 Rings

Support zero or more ring bands:

```text
RingSystem
  enabled
  plane_mode: EQUATORIAL | CUSTOM
  plane_tilt / node
  shadow_strength
  bands[]

RingBand
  inner_radius_m
  outer_radius_m
  optical_depth
  density_curve
  albedo/color
  roughness
  particle_scale_range
  gap/noise profile
  texture refs
```

A band list is preferable to one monolithic texture because named gaps and multiple materials can be authored directly. A generated radial density texture can still be produced for runtime efficiency.

---

## 6. Celestials category

Build a hierarchy + orbital map, not just a body dropdown.

### Hierarchy operations

- create star;
- create planet;
- create moon;
- duplicate;
- rename;
- reparent;
- switch active body;
- focus body in map;
- delete.

Deletion must be dependency-aware:

- if a body has children, offer **delete subtree** or **reparent children**;
- if deleting the active body, require selecting a fallback body first;
- never leave dangling `parent_body_id` references.

### Orbital map

- orbit ellipses generated from the same stored orbital elements used by simulation;
- hierarchical body tree synchronized with map selection;
- logarithmic/adaptive zoom due to huge scale differences;
- periapsis/apoapsis markers;
- ascending-node marker;
- ring extents;
- active body indicator;
- play/scrub time later.

Do not maintain a second independent set of "pretty map" orbital parameters.

---

## 7. Terrain category

The Terrain category has two different editing modes:

1. **surface authoring** — biome paint and masks;
2. **shader authoring** — displacement/material slots + graphs.

### 7.1 Post-generation biome painting

Generated biome remains an immutable base. Add a sparse user override layer.

Suggested resolved model per authoring texel:

```text
base_biome_id
user_biome_id
user_override_weight 0..1
```

Important: do **not** linearly interpolate numeric biome IDs. Resolve categorical IDs first and use a separate continuous edge/mask weight for smooth visual transitions.

Brush controls:

- biome picker;
- radius;
- hardness;
- opacity/strength;
- paint;
- erase/reveal generated biome;
- eyedropper;
- fill connected region later;
- optional noise/jitter mask;
- optional altitude/slope restriction.

Storage:

- sparse cube-face/spherical tiles;
- only allocate edited tiles;
- stroke stores before/after tile diffs for undo;
- dirty tile region invalidates only affected context/scatter/water dependencies.

Runtime integration order:

```text
generated biome
  -> user biome override resolver
  -> GPU PlanetContext resolved biome/masks
  -> terrain material graph biome gates
  -> scatter ecology gates
  -> other biome consumers
```

This ensures biome paint changes both the look and the ecosystem rather than only recoloring terrain.

### 7.2 Displacement slot stack

Separate from material slots.

Each `TerrainDisplacementSlot` contains:

```text
slot_id
name
enabled
order
graph_id
strength
blend_mode
clipmap_level_mask
biome_selector_mode
biome_mask
optional author_mask_id
optional altitude range
optional slope range
physics_mode
```

#### Clipmap targeting

Support:

- all levels;
- explicit L0...L14 checkboxes;
- contiguous min/max level;
- convenience presets such as Near / Mid / Far.

The graph also receives `clipmap_level`, `vertex_spacing`, and camera distance, so a graph can vary continuously inside its allowed set.

#### Biome targeting

Selector modes:

- `ALL`;
- `ONLY_SELECTED`;
- `ALL_EXCEPT_SELECTED`.

Use an 18-biome bitset/mask initially. The UI shows named biome chips/check boxes.

#### Displacement composition

Minimum blend modes:

- ADD;
- SUBTRACT;
- MULTIPLY_GAIN;
- MIN;
- MAX;
- REPLACE;
- MASKED_REPLACE.

Signed ADD/SUBTRACT is enough for real constructive/destructive interference: two overlapping wave/noise graphs are evaluated as signed height fields and summed. A graph output does not need an artificial "interference" special effect.

Example:

```text
L0-L2: rock microfracture +0.06 m
L0-L3: sediment ripple +/-0.03 m
L0-L1: tyre/editor authored displacement -0.02 m
```

All three are compiled into the same final displacement function for those levels.

#### Physics contract

Every displacement graph must declare one of:

- `VISUAL_ONLY`;
- `CONTACT_COMPATIBLE`;
- `PHYSICS_AUTHORITATIVE`.

Arbitrary visual shader code must **not** silently change collision. A physics-authoritative graph has to compile to/evaluate through the same restricted graph IR in the contact query path. The editor should warn visibly when a surface is visually displaced but physics does not follow it.

### 7.3 Material slot stack

Same targeting model as displacement:

```text
TerrainMaterialSlot
  graph_id
  enabled/order
  opacity
  clipmap_level_mask
  biome selector
  optional authored mask
  distance/slope/altitude gates
```

Material outputs:

- base color;
- roughness;
- metallic;
- normal;
- ambient occlusion;
- emission;
- optional height/parallax channel later.

Default layer composition should remain PBR-safe:

- base color: masked mix;
- roughness/metallic: masked mix;
- AO: masked mix/multiply depending node;
- normals: reoriented/robust normal blend, not naïve RGB addition;
- emission: additive after mask.

Exotic blend modes can be added, but the default authoring path should not generate physically nonsensical materials accidentally.

---

## 8. Terrain node editor

### 8.1 Use a custom GraphEdit-based editor

Godot 4.7 `GraphEdit` is designed for graph-like tools and provides node movement, connection requests, minimap, zoom, copy/paste hooks, etc. `GraphNode` supports typed left/right ports.

Use these controls for the UI, but keep the authoritative graph as a separate serialized data model. UI `GraphNode`s are views of graph data, not the saved graph itself.

### 8.2 Graph model

```text
ShaderGraphResource
  graph_id
  domain: DISPLACEMENT | MATERIAL
  version
  nodes[]
  links[]
  parameters[]
  editor_layout

GraphNodeData
  node_id
  type_id
  properties
  position

GraphLinkData
  from_node/from_port
  to_node/to_port
```

Port types initially:

- `FLOAT`;
- `BOOL`;
- `INT`;
- `VEC2`;
- `VEC3`;
- `COLOR`;
- `NORMAL`;
- `TEXTURE2D` reference;
- `MASK` scalar semantic.

The compiler owns compatibility/conversion rules.

### 8.3 Asterra input nodes

#### Geometry / clipmap

- planet direction;
- latitude / longitude;
- local tangent coordinates;
- canonical/detail position;
- generated macro height;
- resolved terrain height before this slot;
- surface normal;
- slope;
- curvature proxy later;
- clipmap level;
- vertex spacing;
- LOD morph;
- camera distance;
- AGL where meaningful;
- deterministic seed/hash.

#### Terrain/environment context

Expose the existing GPU context fields:

- biome ID;
- resolved biome soft weight/mask;
- temperature;
- precipitation;
- moisture;
- vegetation biomass;
- soil sand/silt/clay/organic/depth;
- sediment/deposition;
- geology/rock family;
- erodibility;
- strata dip;
- fault/uplift fields where available;
- hydrology flow direction;
- discharge;
- wetland/deposition;
- authored paint masks.

#### Water context

- ocean/lake/river coverage;
- water feature type;
- water surface height;
- water depth;
- shoreline signed distance;
- current direction;
- current speed;
- turbulence/foam field.

#### World/weather/celestial

- simulation time;
- day phase;
- year/season phase;
- sun direction;
- sun elevation;
- local wind vector/speed;
- cloud/rain/wetness values when available;
- body radius;
- gravity when useful.

### 8.4 Processing nodes

First useful library:

- constants / exposed parameters;
- add/subtract/multiply/divide;
- min/max;
- abs/sign;
- power/root;
- clamp/saturate;
- remap/map range;
- step/smoothstep;
- mix;
- scalar/vector compose/split;
- dot/cross/normalize/length;
- curve mapping;
- color ramp;
- deterministic value noise;
- fractal noise;
- ridge noise;
- periodic noise;
- texture sample;
- channel split/combine;
- UV scale/rotate/offset;
- triplanar projection;
- height/slope/climate/soil/biome masks;
- distance/gradient;
- normal blend;
- height-to-normal helper;
- switch/select;
- reroute;
- comment/frame.

Later:

- Voronoi/cellular;
- domain warp;
- erosion-like procedural masks;
- custom expression node, only after validation/sandboxing is mature.

### 8.5 Output nodes

Displacement graph:

```text
signed_height_delta_m
optional detail_normal
optional mask/debug output
```

Material graph:

```text
base_color
roughness
metallic
normal
AO
emission
```

### 8.6 Graph compiler

Do not directly concatenate arbitrary user text.

Pipeline:

```text
serialized graph
 -> validate ports/types
 -> cycle detection / DAG order
 -> domain validation
 -> constant folding
 -> dead-node elimination
 -> common subexpression opportunities later
 -> lower to typed IR
 -> generate deterministic shader source
 -> hash generated source
 -> compile
 -> if success: stage as preview/active candidate
 -> if failure: retain last-known-good shader and show node/compiler error
```

Generate functions/includes that plug into the authoritative terrain shader, for example conceptually:

```text
float authored_displacement(Context c, int level)
MaterialState authored_material(Context c, MaterialState base)
```

Slots are compiled into those functions. They are **not** separate materials/passes.

### 8.7 Performance budget inspector

The editor should estimate graph cost before Apply:

- texture samples;
- noise evaluations/octaves;
- high-cost transcendental functions;
- branches;
- active clipmap levels;
- number of enabled slots;
- approximate near/mid/far cost.

Quality profiles can impose caps. For a GTX 1050-class profile, expensive graphs should either be rejected on far clipmaps or automatically receive a reduced compiler variant.

Potential later optimization: compile Near/Mid/Far shader variants with dead slot/node elimination for each range.

---

## 9. Texture asset import

The graph editor needs a managed asset library, not raw absolute OS paths.

Import workflow:

1. `FileDialog` selects an image.
2. Load/validate image.
3. Hash content.
4. Copy into the active authoring project's managed asset store.
5. Create `TextureAssetDefinition` metadata.
6. Graph nodes reference the asset ID, not the external path.

Metadata:

```text
asset_id / content hash
source/display name
usage: ALBEDO | NORMAL | ROUGHNESS | AO | HEIGHT | MASK | GENERIC
color_space: SRGB | LINEAR
normal_convention: OPENGL | DIRECTX
physical_scale_m
wrap mode
filter mode
mip policy
channel mapping
alpha meaning
max runtime resolution
compression preference
```

For a developer build, add an optional **Pack into project** action. The portable preset format should carry/copy its managed dependencies so another machine does not need the original desktop path.

---

## 10. Water category

Water authoring should store **vector features** and rasterize them into a local GPU water field used by the same clipmap geometry family as the ocean.

Do not permanently tessellate every river into independent high-detail meshes.

### 10.1 WaterFeatureSet

```text
WaterAuthoringProfile
  ocean_profile
  lakes[]
  rivers[]
  shared_wave_profiles[]
  shared_material_profiles[]
```

### 10.2 Rivers — spherical cubic Bézier features

Godot `Curve3D` is useful as the local editing proxy, but canonical control points should be stored in planetary coordinates suitable for double precision.

Suggested persistent control point:

```text
RiverControlPoint
  surface_direction
  elevation_m / auto_surface flag
  tangent_in_m in local tangent basis
  tangent_out_m in local tangent basis
  width_m
  depth_m
  discharge/current controls
```

At runtime/editor proximity, construct a local `Curve3D`/`Path3D` proxy around the floating origin.

River properties:

- width curve;
- depth curve;
- bank width/falloff;
- bed shape;
- automatic downhill surface profile with manual override handles;
- source/sink linkage;
- discharge;
- current speed profile;
- current direction;
- roughness;
- turbulence;
- foam threshold;
- wave attenuation;
- water material/wave profile;
- optional non-destructive terrain carve modifier.

The UI should prevent accidental uphill water unless explicitly overridden, but authored waterfalls/locks should remain possible.

### 10.3 Lakes — freeform spherical polygons

Persistent polygon vertices:

```text
LakeDefinition
  boundary_directions[]
  water_level_m
  depth/default bathymetry
  shore_falloff
  material/wave profile
  current profile
  connected river IDs
```

Editing flow:

- click points on terrain to create polygon;
- drag/add/delete vertices;
- close polygon;
- local tangent projection for editing/triangulation preview;
- store boundary back as unit directions rather than global float32 points.

For large polygons/cube-face crossings, split only the runtime rasterization chunks; keep one spherical logical feature.

### 10.4 Unified water field clipmap

Generalize the current ocean clipmap so the clipmap samples a **water domain field**.

Minimum field channels:

```text
coverage 0..1
surface_height_m
depth_m
shore_signed_distance_m
current_tangent_x
current_tangent_y
feature/settings ID
foam/turbulence
```

Composition:

```text
Ocean: coverage + sea-level surface
Lake: polygon coverage + lake level
River: spline signed distance + longitudinal surface-height profile
```

A compute rasterizer updates only dirty/visible water field tiles when a lake/river changes or the clipmap window moves.

The water geometry clipmap then becomes:

```text
clipmap vertex
 -> water field lookup
 -> discard/fade outside coverage
 -> base surface height
 -> wave/simulation displacement
 -> shared water material/refraction/lighting
```

This satisfies the goal of keeping ocean/lake/river on the same clipmap **system** while allowing their base geometry and flow to differ.

### 10.5 Wave/simulation modes

Use one geometry/field architecture but allow solver/profile types:

- Ocean: existing wind/Gerstner/spectral-style stack;
- Lake: attenuated wind waves plus optional shallow-water interaction;
- River: current-dominant shallow-water/advection surface with small wind waves;
- user-selectable simplified mode for low-end hardware.

The same current field should feed:

- visual advection;
- foam;
- buoyancy/water-physics query;
- floating objects;
- later sediment/erosion coupling.

Do not create a visual-only current vector unrelated to physics.

### 10.6 Terrain carving

River/lake bed carving should initially be a named authoring modifier layer, not baked directly into gameplay `Deltas`.

Example:

```text
WaterCarveModifier
  feature_id
  enabled
  signed depth profile
  bank falloff
```

It joins the terrain displacement stack before player deformation. It can be toggled, undone, and re-authored. A later explicit **Bake** command can destructively commit it if needed.

---

## 11. Atmospheric category

Separate **visual atmosphere** from **climate-generation parameters**.

### Visual/hot-preview controls

- atmosphere top height;
- Rayleigh scattering strength/scale height;
- Mie/aerosol strength/scale height;
- Mie anisotropy;
- absorber/ozone controls;
- ground/planet albedo coupling;
- aerial perspective strength;
- haze/fog;
- sun disc apparent size/intensity;
- exposure/tone-map defaults;
- cloud base/top;
- coverage;
- density;
- cloud noise scales;
- wind/advection;
- cloud lighting/shadow controls;
- precipitation visual parameters.

### Climate/rebuild controls

Existing generator climate inputs such as solar constant, greenhouse offset, diffusion, lapse rate, precipitation, axial tilt, etc. should be visible under a separate **Climate generation** foldout and marked as requiring a climate/world rebake.

The editor should never make a costly climate rebake look like a harmless color slider.

---

## 12. Apply transaction and invalidation system

Every property/operation is tagged with an apply scope:

```text
HOT_UNIFORM
GRAPH_RECOMPILE
CONTEXT_TILE_UPDATE
WATER_FIELD_UPDATE
CLIPMAP_REBUILD
PLANET_REBAKE
CELESTIAL_REBIND
```

When Apply is pressed:

1. validate staged model;
2. validate body hierarchy/orbits;
3. validate graph topology/types;
4. compile changed graphs to candidate shaders;
5. verify required texture assets;
6. calculate dirty biome/water tiles;
7. calculate maximum apply scope;
8. snapshot currently active runtime state;
9. commit smallest required updates;
10. if any critical step fails, restore prior state;
11. mark session clean only after success.

Avoid `build_roots()`/full rebake for ordinary graph or biome changes.

### Body switching

Switching active planets must explicitly invalidate/rebind:

- `Planet` active data;
- `Frames` body radius/frame data;
- PlanetContext textures;
- terrain cache generation;
- rendered/contact queries;
- terrain edit/authoring overlays;
- biome override resources;
- scatter ecology state;
- water clipmap/field;
- ocean physics;
- atmosphere/cloud profile;
- celestial lighting inputs.

A cache key must include `body_id` or a body generation UUID. Never allow one planet's cache tile to be treated as valid for another planet because the integer cache generation happens to match.

---

## 13. Undo/Redo design

Use Godot `UndoRedo`, wrapped by an authoring command layer.

One semantic operation = one history action:

- one slider drag = one coalesced property action;
- one biome brush stroke = one action;
- one node connection/disconnection = one action;
- moving multiple selected graph nodes = one action;
- one river handle drag = one action;
- one lake vertex edit = one action;
- body create/delete/reparent = one action;
- texture import = one action when practical.

Biome strokes should store sparse tile diffs rather than whole-planet copies.

A body deletion undo record must preserve the body plus affected child-parent relationships.

Save/load is not implemented as a fake undo operation; loading a preset creates a clear new session state/history boundary.

---

## 14. Presets and project persistence

Support three preset scopes:

1. **System preset** — entire celestial system + all body profiles.
2. **Planet preset** — one body + generation/terrain/water/atmosphere/rings.
3. **Graph preset** — reusable displacement or material graph/slot group.

Suggested authoring workspace:

```text
user://world_authoring/
  projects/<project_id>/
    system.tres
    bodies/
    graphs/
    biome_overrides/
    water/
    assets/<content_hash>/
  presets/
```

Later add a portable package/export format containing the profile plus its referenced texture assets.

Autosave a crash-recovery session separately from explicit presets.

---

## 15. Diagnostics the editor should have from the start

- current body ID / profile version;
- staged vs applied state;
- dirty apply scopes;
- shader compile status + source hash;
- graph cost estimate;
- texture memory estimate;
- biome override tile count;
- water feature/dirty-tile count;
- active clipmap levels;
- selected slot's effective LOD/biome coverage;
- toggle to visualize slot mask;
- toggle to visualize resolved biome override;
- toggle to visualize water coverage/current vectors;
- explicit warning for visual-only displacement versus contact physics;
- last Apply error and last-known-good shader/profile ID.

These tools will prevent a large class of "which height/material path is wrong?" debugging later.

---

## 16. Proposed code layout

```text
scripts/world_authoring/
  world_authoring_editor.gd
  world_authoring_session.gd
  world_authoring_apply.gd
  world_authoring_undo.gd

  model/
    celestial_system_definition.gd
    celestial_body_definition.gd
    planet_authoring_profile.gd
    orbit_definition.gd
    ring_system_definition.gd
    terrain_authoring_profile.gd
    atmosphere_profile.gd
    water_authoring_profile.gd

  celestial/
    celestial_registry.gd
    active_body_runtime.gd
    orbital_map.gd

  biome/
    biome_override_store.gd
    biome_brush.gd
    biome_override_gpu.gd

  water/
    water_feature.gd
    river_definition.gd
    lake_definition.gd
    water_feature_editor.gd
    water_field_clipmap.gd
    water_field_rasterizer.gd

  graph/
    shader_graph_resource.gd
    shader_graph_node_data.gd
    shader_graph_link_data.gd
    shader_graph_registry.gd
    shader_graph_validator.gd
    shader_graph_ir.gd
    shader_graph_compiler.gd
    shader_graph_cost.gd
    shader_graph_view.gd
    shader_graph_node_view.gd

  assets/
    authoring_asset_library.gd
    texture_asset_definition.gd
    texture_importer.gd

scenes/world_authoring/
  PlanetStudio.tscn
  graph/
  inspectors/
  gizmos/

shaders/generated/        # developer/debug cache only; canonical graph remains data
shaders/world_authoring/  # stable compiler templates/includes
```

Runtime-facing modules should prefer explicit script/resource dependencies where practical rather than adding unnecessary coupling to the global class registry, especially while the current headless script-chain CI is being repaired.

---

## 17. Implementation phases

### Phase 0 — data model + Planet Studio shell

Goal: safe infrastructure, no terrain visual behavior change.

- `CelestialSystemDefinition` / body/profile resources;
- migrate current Asterra definition into a one-body system;
- staged `WorldAuthoringSession`;
- top toolbar Save/Load/Undo/Redo/Apply/Revert;
- five required category pages;
- dirty/apply-scope system;
- versioned persistence;
- crash-recovery autosave;
- basic property inspector framework.

**Acceptance:** editor opens and round-trips the current planet profile without changing the rendered planet.

### Phase 1 — Planet + Celestials

- body hierarchy;
- orbital data model;
- rotation/day controls;
- radius/mass/gravity controls;
- celestial map;
- create/duplicate/delete/reparent;
- active-body switching facade;
- full invalidation on switch;
- first ring band renderer/profile.

**Acceptance:** create a new planet/moon, set its orbit/rotation, switch to it, switch back, and save/load the system without stale terrain/water caches crossing bodies.

### Phase 2 — biome override painting

- sparse override tile storage;
- spherical brush/gizmo;
- biome picker/erase/opacity/hardness;
- resolved biome context path;
- terrain material consumes resolved biome;
- scatter consumes resolved biome;
- localized invalidation;
- stroke undo/redo.

**Acceptance:** paint an isolated biome patch after generation and see terrain + ecological scatter agree at the same boundary without a full planet rebake.

### Phase 3 — water authoring

- spherical river/lake data structures;
- 3D Bézier river handles/proxies;
- freeform lake polygon tool;
- water field clipmap/rasterizer;
- ocean shader generalized to water-domain coverage/height;
- per-feature wave/material profile;
- river current field;
- water physics queries consume same field;
- non-destructive bed carve modifier.

**Acceptance:** draw a river and a lake, edit their shape live, have both render through clipmap water, and measure current/buoyancy from the same authored data.

### Phase 4 — displacement graph + slot compiler

- custom typed GraphEdit UI;
- graph resource and compiler IR;
- Asterra context input nodes;
- core scalar/vector/noise/mask nodes;
- displacement output;
- slot stack;
- clipmap masks;
- biome ALL/ONLY/EXCEPT masks;
- additive/subtractive/min/max composition;
- visual/contact compatibility flags;
- compile validation + last-known-good rollback;
- graph cost inspector.

**Acceptance:** place two displacement graphs on the same clipmap levels, target selected biomes, and visibly produce signed constructive/destructive overlap with one terrain draw path.

### Phase 5 — material graph + managed textures

- texture importer/library;
- texture/sample/triplanar nodes;
- color/roughness/metallic/AO/normal outputs;
- robust normal blending;
- material slot stack with same clipmap/biome selectors;
- portable graph presets and dependencies;
- near/mid/far quality compilation optimization.

**Acceptance:** import a texture set, construct a graph, restrict it to selected biomes/LODs, stack it with a second material graph, save as preset, reload and get bit-equivalent graph state.

### Phase 6 — atmosphere authoring

- move hard-coded/runtime visual atmosphere controls into `AtmosphereProfile`;
- live preview;
- clouds/fog/aerial controls;
- climate-generation foldout with rebuild labeling;
- body switching binds the correct profile.

**Acceptance:** two planets can have visibly different atmospheres/cloud profiles and switch without rebaking terrain.

### Phase 7 — hardening/performance/polish

- graph quality-tier variants;
- compiler optimization;
- profile migration tests;
- large lake/river seam tests;
- body-switch stress test;
- authored tile compression;
- editor profiler;
- dependency/package export;
- richer node library;
- optional custom node SDK only after core graph safety is stable.

---

## 18. What should not be done

- Do not replace the current gameplay `TerrainEditor` with the authoring editor.
- Do not mutate pristine generated biome arrays directly when painting.
- Do not store planetary river/lake geometry as global float32 `Vector3`s.
- Do not interpolate numeric biome IDs to make edges smooth.
- Do not implement one draw pass per material/displacement slot.
- Do not let visual displacement silently diverge from physics without an explicit warning/contract.
- Do not rebuild the entire planet for a material uniform, graph parameter, biome stroke, or river handle move.
- Do not use absolute desktop texture paths as persistent preset dependencies.
- Do not allow body switching without clearing/rebinding body-specific cache generations.
- Do not expose only periapsis/apoapsis and throw away the orientation/epoch elements needed to define the orbit.
- Do not let a bad graph compile replace the currently working terrain shader.

---

## 19. Research references

Godot 4.7 APIs selected for the implementation:

- GraphEdit: https://docs.godotengine.org/en/4.7/classes/class_graphedit.html
- GraphNode: https://docs.godotengine.org/en/4.7/classes/class_graphnode.html
- UndoRedo: https://docs.godotengine.org/en/4.7/classes/class_undoredo.html
- Curve3D: https://docs.godotengine.org/en/4.7/classes/class_curve3d.html
- Path3D: https://docs.godotengine.org/en/4.7/classes/class_path3d.html
- Geometry2D: https://docs.godotengine.org/en/4.7/classes/class_geometry2d.html
- Image: https://docs.godotengine.org/en/4.7/classes/class_image.html
- Shader: https://docs.godotengine.org/en/4.7/classes/class_shader.html

Orbital model reference:

- NASA Basics of Space Flight, Chapter 5 — Planetary Orbits: https://science.nasa.gov/learn/basics-of-space-flight/chapter5-1/

---

## 20. Recommended next implementation move

Start with **Phase 0 only** and deliberately avoid touching the production terrain shader in the first pass.

The first code milestone should create the data model, editor shell, staging/apply/undo/preset infrastructure, and import the current single Asterra body into the new system. Once that foundation round-trips safely, implement Planet/Celestials, then biome painting, then water, then the graph compiler.

This order is intentional: the shader graph and water editor depend heavily on correct persistent body/profile IDs, transactions, undo, body switching, and asset ownership. Building those foundations after the graph editor would force a rewrite.