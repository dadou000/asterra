class_name BodyRuntime
extends RefCounted
## One planetary body's detailed-terrain stack.
##
## Seamless multi-planet travel (see the design doc) turns the single-body
## terrain singleton into a small pool of these. In M0 the *primary* body's
## runtime simply holds references to the existing autoload nodes -- `Planet`,
## `PlanetContext`, `Deltas`, `GroundGeometryClipmap`, `OceanSystem` -- and every
## consumer keeps talking to those singletons directly, so behaviour is
## byte-identical to before the extraction. Later milestones (M1+) let non-primary
## bodies own their own instances of that stack and route the singletons through
## `Bodies.active`.
##
## Slot lifecycle (used from M3 on; in M0 the primary is always HOT):
##   COLD  -- nothing resident; deltas serialised out.
##   FAR   -- rendered only as a lit relief sphere (celestial_body_preview_runtime).
##   WARM  -- bake loaded + PlanetContext textures resident, clipmap built but
##            hidden; no page streaming, no collision.
##   HOT   -- full clipmap + collision + ocean + scatter; walkable.

enum State { COLD, FAR, WARM, HOT }

const TERRAIN_DELTAS := preload("res://scripts/terrain/terrain_deltas.gd")
const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")
const _PLANET_BAKE := preload("res://scripts/gen/planet_bake.gd")
const _PLANET_SAMPLER := preload("res://scripts/gen/planet_sampler_gpu_runtime.gd")
## Concurrent (non-active) bodies render with the procedural-GPU clipmap layer:
## context textures + orbit-elevation + biome shader, NO geomorph compute cache.
## That is the right LOD for a body you are only viewing from afar and avoids
## contending for the shared RenderingDevice with the active body's cache. The
## full phase31 stack (with the cache) is reserved for the active body.
const _CLIPMAP_SCRIPT := preload("res://scripts/terrain/spherical_geometry_clipmap_global_gpu.gd")

## Radial-gravity fallback when a body carries no authored surface gravity. Matches
## the historical single-planet constant (player.gd / physics_walker_body.gd).
const FALLBACK_SURFACE_GRAVITY := 9.62

## Stable identifier for this body within the celestial system. The standalone
## game's resident body is "asterra"; generated systems name theirs from the
## CelestialBodyDefinition.body_id.
var id: StringName = &"asterra"
var state: State = State.HOT
## True for the one runtime wired to the resident terrain autoloads.
var is_primary: bool = false

## The per-body stack. For the primary body in M0 every one of these IS the
## corresponding autoload node; non-primary runtimes get their own instances in
## later milestones. Typed as Node so both cases assign cleanly.
var sampler: Node = null    ## Planet-equivalent (planet_sampler_gpu_runtime)
var context: Node = null    ## PlanetContext-equivalent (gpu_planet_context)
var deltas: Node = null     ## terrain edit store (terrain_deltas)
var clipmap: Node = null    ## GroundGeometryClipmap-equivalent
var ocean: Node = null      ## OceanSystem-equivalent
## A second live clipmap instance rendering THIS body's detailed terrain while
## another body is active (M5b). Null unless promoted concurrent.
var concurrent_clipmap: Node = null
## Monotonic seconds when this runtime was last "relevant" (warmed / promoted /
## active / contact). The pool evicts the least-recently-relevant WARM body when
## it exceeds the warm-slot budget (M6 LRU).
var last_touched_s: float = 0.0

## --- Swap lifecycle state (M3) --------------------------------------------
## This body's GenConfig (bake input). Primary: the Planet autoload's cfg.
var gen_config: Resource = null
## Loaded PlanetFields (a cached PlanetBake load is instant). Null until warmed.
var fields: PlanetFields = null
## Body centre in the system frame (root star at the origin), metres.
var center_system: Vec3D = Vec3D.new()
## Reference-sphere radius (m): cloud tops for a gas giant, the body's own radius
## otherwise. Orbits / far-LOD / camera framing / the approach state machine use
## this. Set authoritatively at register_body; NOT overwritten by warm().
var radius_m: float = 1_000_000.0
## The radius the resident terrain bake / collision / altitude datum key off. For
## a gas giant this is its solid core (rho ~= 1000 kg/m^3); == radius_m otherwise.
## 0 means "same as radius_m".
var surface_radius_m: float = 0.0
## Authored surface gravity (m/s^2); <= 0 means use FALLBACK_SURFACE_GRAVITY.
var surface_gravity_m_s2: float = 0.0
## Serialised terrain edits captured when this body last went cold, replayed into
## the shared Deltas store when it next becomes active.
var _delta_blob: Dictionary = {}


func _init(p_id: StringName = &"asterra") -> void:
	id = p_id


## Planet-centre radius datum in metres. For the primary body this tracks the live
## `Planet` autoload cfg; for non-primary bodies it is `radius_m` (set at register).
func radius() -> float:
	if sampler != null:
		var cfg: Variant = sampler.get("cfg")
		if cfg != null:
			return float(cfg.planet_radius)
	return radius_m


func is_hot() -> bool:
	return state == State.HOT


## The bake / collision / altitude datum radius (core for a gas giant, else the
## reference radius).
func surface_radius() -> float:
	return surface_radius_m if surface_radius_m > 0.0 else radius_m


## Build this runtime's own coarse GPU context (GPUPlanetContext), following
## `sampler` (its own sampler node) rather than the `Planet` autoload. Primary
## bodies skip this and keep pointing `context` at the `PlanetContext` autoload.
## The returned node is not parented -- the caller adds it to the tree.
func make_context(sampler: Node) -> Node:
	var ctx: Node = GPUPlanetContext.new()
	ctx.name = "PlanetContext_%s" % id
	ctx.call(&"bind_sampler", sampler)
	context = ctx
	return ctx


## Build this runtime's own terrain-edit store. Primary bodies use the `Deltas`
## autoload; non-primary bodies get an isolated instance so an edit or a bake on
## one body can never corrupt another body's heightfield (the Planet Studio
## moon-bake corruption). The returned node is not parented.
func make_deltas() -> Node:
	var store: Node = TERRAIN_DELTAS.new()
	store.name = "Deltas_%s" % id
	deltas = store
	return store


## Radial gravity magnitude (m/s^2) at this body's surface.
func surface_gravity() -> float:
	return surface_gravity_m_s2 if surface_gravity_m_s2 > 0.0 else FALLBACK_SURFACE_GRAVITY


## --- Swap lifecycle (M3) --------------------------------------------------
## COLD/FAR -> WARM: load this body's baked fields (a cached PlanetBake load is
## instant; a cold bake is a CPU Thread, no GPU) and pre-build its coarse GPU
## context so a later `activate` has nothing expensive left to do. Idempotent.
func warm() -> bool:
	if is_primary:
		# The primary body's stack is the always-resident singleton -- nothing to
		# bake or rebuild, and it must not occupy a WARM budget slot. No-op.
		return true
	if state >= State.WARM and fields != null:
		return true
	if gen_config == null:
		push_warning("BodyRuntime(%s).warm: no gen_config" % id)
		return false
	if fields == null:
		var bake := _PLANET_BAKE.new(gen_config)
		fields = bake.bake(Callable(), true)  # cached load is instant; else CPU bake + cache
	if fields == null:
		return false
	# The bake config's radius is the SURFACE datum (the core for a gas giant).
	# radius_m (the reference / cloud-top radius) is owned by register_body.
	surface_radius_m = float(gen_config.planet_radius)
	if radius_m <= 1.0:
		radius_m = surface_radius_m
	if context == null:
		context = GPUPlanetContext.new()
		context.name = "PlanetContext_%s" % id
	if context.has_method(&"build_from_fields"):
		context.call(&"build_from_fields", fields)
	state = State.WARM
	return true


## WARM -> HOT on the shared singleton stack: adopt this body's fields into the
## `Planet` autoload (which fires `world_ready`, rebuilding the resident
## PlanetContext / clipmap / ocean around this body) and replay this body's saved
## terrain edits. The caller (main.gd) is responsible for the surrounding
## `_on_baked`-style refresh (build_roots / orbit textures / editor) and for
## re-seating the player. Frames.planet_radius is set by Planet.adopt.
func activate_singleton() -> void:
	if fields == null:
		warm()
	if fields == null:
		push_error("BodyRuntime(%s).activate_singleton: no fields" % id)
		return
	var planet: Node = _autoload(&"Planet")
	var deltas_store: Node = _autoload(&"Deltas")
	if deltas_store != null:
		if _delta_blob.is_empty():
			deltas_store.call(&"clear")
		else:
			deltas_store.call(&"deserialize", _delta_blob)
	if planet != null:
		planet.call(&"adopt", fields)
	state = State.HOT


## HOT -> FAR: capture this body's terrain edits from the shared store and drop
## the resident GPU context. `fields` is kept (a cheap RefCounted) so a re-approach
## re-warms instantly.
## `capture_edits` MUST be true only when this body currently owns the shared
## `Deltas` store (i.e. it is the body being swapped away from). A warm body
## evicted by the pool budget is NOT the active body -- capturing then would
## overwrite its real blob with the active body's live edits.
func cold(capture_edits: bool = true) -> void:
	if capture_edits:
		var deltas_store: Node = _autoload(&"Deltas")
		if deltas_store != null:
			_delta_blob = deltas_store.call(&"serialize")
	if context != null and is_instance_valid(context) and not is_primary:
		context.free()
		context = null
	state = State.FAR


## Build this runtime's own Planet sampler node (a non-primary body needs one to
## render a concurrent detailed clipmap, M5). Not parented -- the caller adds it.
func make_sampler() -> Node:
	if sampler != null and is_instance_valid(sampler) and not is_primary:
		return sampler
	var s: Node = _PLANET_SAMPLER.new()
	s.name = "Sampler_%s" % id
	if gen_config != null:
		# planet_sampler.configure() writes Frames.set_planet_radius -- a non-active
		# body must not disturb the canonical frame datum.
		var frames := _autoload(&"Frames")
		var keep: float = float(frames.get(&"planet_radius")) if frames != null else 0.0
		s.call(&"configure", gen_config)
		if frames != null and keep > 0.0 and not is_primary:
			frames.call(&"set_planet_radius", keep)
	sampler = s
	return s


## Adopt this body's baked fields into its OWN sampler + context without the
## `Planet.adopt` side effect of retargeting `Frames.planet_radius` -- the active
## body still owns the canonical frame. Used to bring up a concurrent (non-active)
## detailed clipmap.
func adopt_fields_silent() -> void:
	if is_primary:
		return
	# Capture the active body's radius BEFORE anything here -- both
	# planet_sampler.configure() and .adopt() write Frames.set_planet_radius.
	var frames := _autoload(&"Frames")
	var keep_radius: float = float(frames.get(&"planet_radius")) if frames != null else 0.0
	if fields == null:
		warm()
	if fields == null:
		return
	var s := make_sampler()
	if context == null:
		make_context(s)
	elif context.has_method(&"bind_sampler"):
		context.call(&"bind_sampler", s)
	s.call(&"adopt", fields)   # sets this sampler's cfg/grid + fires its world_ready
	if frames != null and keep_radius > 0.0:
		frames.call(&"set_planet_radius", keep_radius)
	if gen_config != null:
		surface_radius_m = float(gen_config.planet_radius)


## Bring up a live second clipmap that renders THIS body's detailed terrain
## alongside the active body's (M5b). Idempotent. `parent` is the node the clipmap
## (+ this body's sampler/context) are added under. Returns the clipmap node.
func promote_concurrent(parent: Node) -> Node:
	if is_primary or parent == null:
		return null
	if concurrent_clipmap != null and is_instance_valid(concurrent_clipmap):
		return concurrent_clipmap
	adopt_fields_silent()   # own sampler + context, no Frames.planet_radius change
	if sampler == null or context == null:
		return null
	if not sampler.is_inside_tree():
		parent.add_child(sampler)
	if not context.is_inside_tree():
		parent.add_child(context)
	var cm: Node = _CLIPMAP_SCRIPT.new()
	cm.name = "ConcurrentClipmap_%s" % id
	cm.call(&"bind_runtime", self, true)   # far_only
	parent.add_child(cm)                   # _ready binds to our sampler + context
	concurrent_clipmap = cm
	state = State.HOT
	return cm


## Tear down the concurrent clipmap (the body leaves the concurrent band, or it
## just became the active body and the singleton stack takes over).
func demote_concurrent() -> void:
	if concurrent_clipmap != null and is_instance_valid(concurrent_clipmap):
		concurrent_clipmap.queue_free()
	concurrent_clipmap = null
	if state == State.HOT and not is_primary:
		state = State.WARM if fields != null else State.FAR


func is_concurrent() -> bool:
	return concurrent_clipmap != null and is_instance_valid(concurrent_clipmap)


## The captured edit blob (for save/inspection). Empty when this body has never
## been active or had no edits.
func delta_blob() -> Dictionary:
	return _delta_blob


func describe() -> String:
	return "BodyRuntime(%s, %s%s)" % [
		id, State.keys()[state], ", primary" if is_primary else ""]


func _autoload(node_name: StringName) -> Node:
	var loop := Engine.get_main_loop()
	if loop == null or not (loop is SceneTree):
		return null
	return (loop as SceneTree).root.get_node_or_null(NodePath(node_name))
