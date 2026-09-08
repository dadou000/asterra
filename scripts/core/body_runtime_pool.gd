extends Node
## Autoload `Bodies`: the pool of BodyRuntime stacks and the notion of which body
## the player is standing on, plus the approach state machine that swaps the
## resident detailed stack from one body to another with no modal load (M3).
##
## M0/M1 registered exactly one runtime -- the primary body wired to the resident
## terrain autoloads. M3 adds `register_body()` for a second body and `update()`,
## which promotes an approached body to WARM (cached bake load + coarse GPU
## context, no streaming/collision) and, once the observer is nearer that body's
## surface than the active one's and above the active body's atmosphere, runs
## `set_active()` -- the `main.gd._on_baked` re-adopt sequence minus the bake and
## minus the forced player teleport.
##
## Declared in project.godot after `GroundGeometryClipmap` so every stack node it
## references already exists when `_ready` runs.

signal active_changed(runtime: BodyRuntime, player_world_pos: Vec3D)

const BODY_RUNTIME := preload("res://scripts/core/body_runtime.gd")

## Approach band (in target-body radii): closer than this and the body is warmed.
const WARM_BAND_RADII: float = 1200.0
## Concurrent-render band (in target-body radii): closer than this and the body
## gets a live second clipmap rendering its detailed terrain alongside the active
## one (M5b). Well inside WARM_BAND so it is always already warm.
const CONCURRENT_BAND_RADII: float = 400.0
## Default atmosphere-top altitude (m) when a body's GenConfig does not specify.
const DEFAULT_ATMOSPHERE_TOP_M: float = 60_000.0

## --- M6: interplanetary cruise / slot budget ------------------------------
## Warm a body the velocity ray is heading for, even before proximity, so a cold
## CPU bake finishes during the (multi-minute) cruise. Trigger when the ray's
## closest approach is within this many radii AND arrival is within the horizon.
const PREFETCH_APPROACH_RADII: float = 6000.0
const PREFETCH_HORIZON_S: float = 900.0
## At most one WARM (bake+context resident, not rendering) body beyond the active
## one and any concurrent body -- the least-recently-relevant extra is dropped.
const MAX_WARM_SLOTS: int = 1
## Hard ceiling on fully-rendered detailed bodies (active + concurrent). The one
## shared main-thread RenderingDevice cannot feed a third clipmap's LOD select +
## page stream + geomorph cache + collision at frame rate (M9). Never raised.
const MAX_HOT: int = 2

## Node that concurrent clipmap instances (+ their sampler/context) are parented
## under. Set by main._activate_system; null in headless tests that drive the pool
## directly (they pass their own parent).
var concurrent_parent: Node = null

## Every registered runtime. M0: [primary]. Grows to 2 HOT + 1 WARM + N FAR.
var slots: Array[BodyRuntime] = []
## The body under the player's feet -- the one the `Planet`/`Frames` datum is for.
var active: BodyRuntime = null
## The body owning collision/contact this frame. Equal to `active` until M5 lets a
## planet+moon pair both run HOT and hands physics to whichever surface is nearer.
var contact_body: BodyRuntime = null

var _primary: BodyRuntime = null
var _swaps: int = 0


func _ready() -> void:
	_ensure_primary()


## Build (once) the primary BodyRuntime that wraps the resident terrain autoloads.
## Idempotent: safe to call from `main._ready` after `Planet.configure`; re-syncs
## the primary's gen_config / fields / radius / frame from the now-ready Planet.
func primary() -> BodyRuntime:
	var rt := _ensure_primary()
	if rt != null and rt.sampler != null and rt.sampler.get(&"cfg") != null:
		rt.gen_config = rt.sampler.get(&"cfg")
		rt.fields = rt.sampler.get(&"fields")
		rt.radius_m = float(rt.sampler.cfg.planet_radius)
		var frames := _frames()
		if frames != null and frames.has_method(&"set_body_frame"):
			frames.call(&"set_body_frame", rt.id, Vec3D.new(), rt.radius_m, rt.surface_radius())
	return rt


func _ensure_primary() -> BodyRuntime:
	if _primary != null:
		return _primary
	var tree := get_tree()
	if tree == null or tree.root == null:
		return null
	var root := tree.root
	var rt := BODY_RUNTIME.new(&"asterra")
	rt.is_primary = true
	rt.state = BODY_RUNTIME.State.HOT
	rt.sampler = root.get_node_or_null(^"Planet")
	rt.context = root.get_node_or_null(^"PlanetContext")
	rt.deltas = root.get_node_or_null(^"Deltas")
	rt.clipmap = root.get_node_or_null(^"GroundGeometryClipmap")
	rt.ocean = root.get_node_or_null(^"OceanSystem")
	var planet := rt.sampler
	if planet != null and planet.get(&"cfg") != null:
		rt.gen_config = planet.get(&"cfg")
		rt.fields = planet.get(&"fields")
		rt.radius_m = float(planet.cfg.planet_radius)
	_primary = rt
	slots = [rt]
	active = rt
	contact_body = rt
	var frames := root.get_node_or_null(^"Frames")
	if frames != null:
		frames.set(&"active_body_id", rt.id)
		if frames.has_method(&"set_body_frame"):
			frames.call(&"set_body_frame", rt.id, Vec3D.new(), rt.radius_m, rt.surface_radius())
	return rt


## Register a non-primary body so the approach state machine can swap to it.
## `center_system` is the body's centre in the system frame (root star at origin);
## `def` is an optional CelestialBodyDefinition (for surface gravity).
func register_body(id: StringName, gen_config: Resource, center_system: Vec3D,
		radius_m: float, surface_gravity_m_s2: float = 0.0,
		surface_radius_m: float = 0.0) -> BodyRuntime:
	var existing := slot(id)
	if existing != null:
		return existing
	var rt := BODY_RUNTIME.new(id)
	rt.state = BODY_RUNTIME.State.FAR
	rt.gen_config = gen_config
	rt.center_system = center_system.dup()
	rt.radius_m = maxf(radius_m, 1.0)
	rt.surface_radius_m = maxf(surface_radius_m, 0.0)
	rt.surface_gravity_m_s2 = surface_gravity_m_s2
	rt.make_deltas()
	slots.append(rt)
	var frames := _frames()
	if frames != null and frames.has_method(&"set_body_frame"):
		frames.call(&"set_body_frame", id, center_system, rt.radius_m, rt.surface_radius())
	return rt


## The runtime for a given body id, or null if it has no slot.
func slot(id: StringName) -> BodyRuntime:
	for s: BodyRuntime in slots:
		if s.id == id:
			return s
	return null


## Radius datum (m) of the body the player is on. Mirrors `Frames.planet_radius`.
func active_radius() -> float:
	return active.radius() if active != null else 0.0


## Radial gravity magnitude (m/s^2) of the body owning contact this frame.
func contact_gravity() -> float:
	return contact_body.surface_gravity() if contact_body != null \
		else BODY_RUNTIME.FALLBACK_SURFACE_GRAVITY


func swap_count() -> int:
	return _swaps


## Advance the approach state machine. Call every frame with the player's canonical
## world position (active body at the origin) and current velocity (m/s, same
## frame). A no-op while only the primary body is registered.
func update(observer_world: Vec3D, velocity: Vec3D = Vec3D.new()) -> void:
	if active == null or slots.size() < 2:
		return
	var frames := _frames()
	if frames == null:
		return

	var now := _now()
	active.last_touched_s = now
	# Approach / swap / contact bands reason about the REFERENCE radius (cloud tops
	# for a gas giant), consistent with world_altitude_over(other) and `margin`.
	# Frames.planet_radius may be the smaller surface datum (a gas giant's core).
	var alt_active := observer_world.length() - active.radius_m
	var atmo_top: float = _atmosphere_top(active)
	var speed: float = velocity.length()

	var best: BodyRuntime = null
	var best_alt: float = 1.0e30
	for rt: BodyRuntime in slots:
		if rt == active:
			continue
		var canon: Vec3D = frames.call(&"body_center_canonical", rt.id)
		var alt := float(frames.call(&"world_altitude_over", observer_world, rt.id))

		# Warm on proximity OR when the velocity ray is heading here and arrival is
		# within the prefetch horizon -- so an interplanetary cold bake (a CPU
		# thread) finishes during the cruise, not at the doorstep (M6).
		var want_warm: bool = alt < rt.radius_m * WARM_BAND_RADII
		if not want_warm and speed > 1.0:
			var to_body: Vec3D = canon.sub(observer_world)
			var along: float = to_body.dot(velocity) / speed   # metres along the ray
			if along > 0.0:
				var closest: Vec3D = observer_world.add(velocity.mul(along / speed))
				var miss: float = closest.sub(canon).length()
				if miss < rt.radius_m * PREFETCH_APPROACH_RADII \
						and along / speed < PREFETCH_HORIZON_S:
					want_warm = true
		if want_warm and rt.state < BODY_RUNTIME.State.WARM:
			rt.warm()
			rt.last_touched_s = now
		elif want_warm:
			rt.last_touched_s = now

		# A body close enough to matter gets a live second clipmap so its detailed
		# terrain is already on screen -- no swap-in when you arrive (M5b). Hard cap
		# at MAX_HOT total (active + concurrent): the shared RenderingDevice cannot
		# feed a third full clipmap at 60 fps (M9).
		if concurrent_parent != null:
			if alt < rt.radius_m * CONCURRENT_BAND_RADII:
				if not rt.is_concurrent() and _hot_count() < MAX_HOT:
					rt.promote_concurrent(concurrent_parent)
				if rt.is_concurrent():
					rt.last_touched_s = now
			elif rt.is_concurrent():
				rt.demote_concurrent()

		if alt < best_alt:
			best_alt = alt
			best = rt

	_enforce_warm_budget(now)
	_enforce_hot_budget(now)

	if best == null:
		contact_body = active
		return
	best.last_touched_s = now

	# Contact / gravity follows whichever body's surface the player is nearer,
	# flipping BEFORE the full active-body swap so a descent onto the moon already
	# has the moon's gravity while the planet is still the render origin (M5).
	contact_body = best if best_alt < alt_active else active

	# Swap only when we are genuinely nearer the candidate's surface than the
	# active body's, and clear of the active body's atmosphere. At an
	# interplanetary midpoint both altitudes are ~AU-scale and this is the single
	# deep-space re-origin of the cruise (M6); near a planet+moon pair it is the
	# small M3/M5 near-swap. Same code, larger offset.
	var margin: float = 2.0 * maxf(active.radius_m, best.radius_m)
	if alt_active > atmo_top and best_alt < alt_active - margin:
		set_active(best, observer_world)


## Keep at most MAX_WARM_SLOTS bodies WARM beyond the active + concurrent ones;
## drop the least-recently-relevant extra back to FAR (its `fields` are kept so a
## re-approach re-warms instantly).
func _enforce_warm_budget(_now_s: float) -> void:
	var warm: Array[BodyRuntime] = []
	for rt: BodyRuntime in slots:
		if rt == active or rt.is_primary or rt.is_concurrent():
			continue
		if rt.state == BODY_RUNTIME.State.WARM:
			warm.append(rt)
	if warm.size() <= MAX_WARM_SLOTS:
		return
	warm.sort_custom(func(a: BodyRuntime, b: BodyRuntime) -> bool:
		return a.last_touched_s < b.last_touched_s)
	for i in range(warm.size() - MAX_WARM_SLOTS):
		warm[i].cold(false)


func _now() -> float:
	return float(Time.get_ticks_msec()) / 1000.0


## Fully-rendered detailed bodies right now: the active body + every concurrent
## clipmap.
func _hot_count() -> int:
	var n := 1 if active != null else 0
	for rt: BodyRuntime in slots:
		if rt != active and rt.is_concurrent():
			n += 1
	return n


## If concurrency ever exceeds MAX_HOT (e.g. two moons enter the band the same
## frame), demote the least-recently-relevant concurrent body.
func _enforce_hot_budget(_now_s: float) -> void:
	while _hot_count() > MAX_HOT:
		var victim: BodyRuntime = null
		for rt: BodyRuntime in slots:
			if rt == active or not rt.is_concurrent():
				continue
			if victim == null or rt.last_touched_s < victim.last_touched_s:
				victim = rt
		if victim == null:
			break
		victim.demote_concurrent()


## Snapshot for the debug HUD / an N-body soak test.
func budget_stats() -> Dictionary:
	var hot := 0
	var warm := 0
	var far := 0
	var concurrent := 0
	for rt: BodyRuntime in slots:
		if rt == active:
			hot += 1
		elif rt.is_concurrent():
			hot += 1
			concurrent += 1
		elif rt.state == BODY_RUNTIME.State.WARM:
			warm += 1
		else:
			far += 1
	return {
		"slots": slots.size(),
		"hot": hot,
		"concurrent": concurrent,
		"warm": warm,
		"far": far,
		"swaps": _swaps,
		"active": String(active.id) if active != null else "",
		"contact": String(contact_body.id) if contact_body != null else "",
		"max_hot": MAX_HOT,
		"max_warm": MAX_WARM_SLOTS,
	}


## Swap the resident detailed stack onto `target`. `observer_world` is the player's
## canonical position at the moment of the swap; it is re-expressed about the new
## body's centre and handed to listeners (main.gd -> player.reseat_to_active_body).
func set_active(target: BodyRuntime, observer_world: Vec3D) -> void:
	if target == null or target == active:
		return
	var frames := _frames()
	var previous := active

	# Player position re-expressed about the target body's centre.
	var new_world_pos := observer_world
	if frames != null and frames.has_method(&"body_center_canonical"):
		new_world_pos = observer_world.sub(frames.call(&"body_center_canonical", target.id))

	# The target just became the active body: its singleton stack takes over, so
	# tear down its concurrent second clipmap (no bake / no rebuild -- the fields
	# and context are already resident).
	if target.is_concurrent():
		target.demote_concurrent()

	if previous != null:
		previous.cold()   # capture its edits so a return trip restores them

	target.activate_singleton()   # Planet.adopt(target.fields) + replay target deltas

	if frames != null:
		if frames.has_method(&"rebase_to_body"):
			frames.call(&"rebase_to_body", target.id, new_world_pos)
		else:
			frames.set(&"active_body_id", target.id)
			frames.call(&"set_planet_radius", target.surface_radius())
			frames.call(&"rebase", new_world_pos)

	active = target
	contact_body = target
	target.state = BODY_RUNTIME.State.HOT
	target.last_touched_s = _now()
	if previous != null and previous != target:
		previous.state = BODY_RUNTIME.State.FAR
	_swaps += 1
	active_changed.emit(target, new_world_pos)


## Load-time activation (M8 save restore): make body `id` active with the player
## already at `player_world_about_id` (a body-local position straight out of the
## save), NOT re-expressed. Runs the same adopt + Frames re-origin + refresh as a
## swap but does not count as a cruise swap and does NOT capture the previous
## body's edits (the save restore has already placed every body's blob).
func load_active(id: StringName, player_world_about_id: Vec3D) -> void:
	var target := slot(id)
	if target == null:
		return
	if target == active:
		# Already the resident body -- just place the player and re-origin.
		var f := _frames()
		if f != null and f.has_method(&"rebase_to_body"):
			f.call(&"rebase_to_body", id, player_world_about_id)
		return
	var previous := active
	if previous != null and previous != target and previous.is_concurrent():
		previous.demote_concurrent()
	target.activate_singleton()
	var frames := _frames()
	if frames != null and frames.has_method(&"rebase_to_body"):
		frames.call(&"rebase_to_body", id, player_world_about_id)
	active = target
	contact_body = target
	target.state = BODY_RUNTIME.State.HOT
	target.last_touched_s = _now()
	if previous != null and previous != target:
		previous.state = BODY_RUNTIME.State.FAR
	active_changed.emit(target, player_world_about_id)


func _atmosphere_top(rt: BodyRuntime) -> float:
	if rt != null and rt.gen_config != null:
		var h: float = float(rt.gen_config.get(&"atmosphere_height"))
		if h > 0.0:
			return h
	return DEFAULT_ATMOSPHERE_TOP_M


func _frames() -> Node:
	var tree := get_tree()
	if tree == null or tree.root == null:
		return null
	return tree.root.get_node_or_null(^"Frames")
