extends Node
## Autoload: hierarchical reference frames + floating origin.
##
##   Helion frame  ->  Asterra frame  ->  regional frame  ->  local physics frame
##                                                          ->  assembly frame
##
## Canonical state is double precision and planet-centred (Asterra frame). The
## Godot scene graph only ever sees the *local physics frame*: a float32 space
## whose origin is periodically re-based to follow the observer, so contact
## physics and rendering always happen within a few kilometres of (0,0,0).
##
## This is the single reason the player can fly from orbit to the ground without
## vertex jitter, and it must exist before any Phase 1 terrain code.

signal origin_shifted(delta_render: Vector3)

const REBASE_THRESHOLD := 4096.0
# Floating-origin changes are global coordinate-frame mutations. Never commit one
# from an arbitrary player's _physics_process callback: render consumers may have
# already published uniforms for that rendered frame. Requests are committed here
# at the very beginning of the next physics tick, before ordinary physics nodes and
# before the following render-process pass.
const REBASE_PHYSICS_PRIORITY := -100000

## --- Helion system (Asterra's star) -------------------------------------------
var helion_dir: Vector3 = Vector3(1, 0.15, 0.3).normalized()  ## sunward, anchor-body frame
## Physical source geometry. These defaults match the existing climate assumption
## of Sol-like irradiance at one astronomical unit. Rendering systems derive the
## apparent solar disc from these values instead of carrying unrelated blur angles.
var helion_radius_m: float = 696340000.0
var helion_distance_m: float = 149597870700.0
var axial_tilt_deg: float = 21.4
var day_seconds: float = 90000.0
var year_days: float = 402.0

## Distance at which solar_distance_scale() == 1.0 (the pre-orbit constant, 1 AU).
## Also the game-path default for helion_distance_m before any orbit advance.
const REFERENCE_HELION_DISTANCE_M := 149597870700.0

## --- System sim clock --------------------------------------------------------
## Canonical simulated time, seconds since the system epoch. Advanced HERE and
## nowhere else when `playing`; every other system only reads it (or scrubs it via
## the Planet Studio "Orbit & seasons" panel). OrbitalMotionRuntime / main.gd read
## this each frame to place Helion; the terrain frame itself never sees it.
var system_time_s: float = 0.0
var time_scale: float = 1.0          ## sim seconds per real second while playing
var playing: bool = false
## The body Frames world space is centred on (terrain/ocean/scatter are all
## relative to this body's centre; its motion through the system never enters
## `origin`). Set by _sync_frames / OrbitalMotionRuntime on the selected body.
var anchor_body_id: String = "asterra"
## Diurnal phase offset (deg) so the sub-solar longitude at system_time_s == 0 is
## authorable; seeded from the anchor body's rotation_phase_at_epoch_deg.
var _rotation_phase0_deg: float = 0.0

## --- Asterra ------------------------------------------------------------------
## Radius datum of the body the player is currently standing on. On a seamless
## active-body swap (Bodies.set_active, see the multi-planet design) this is set
## to the new body's radius alongside a `rebase` onto the player's position
## re-expressed about that body's centre.
var planet_radius: float = 1000000.0

## --- Per-body sub-frames ----------------------------------------------------
## body_id -> { "center": Vec3D (system frame, root star at origin), "radius": float }.
## Populated by Bodies / OrbitalMotionRuntime. The *active* body's centre is the
## canonical origin (0,0,0); these let transition code ask "how high is this
## canonical point above body X" while X is not yet active.
var _body_frames: Dictionary = {}
## body_id -> surface datum radius (m). For a gas giant this is its solid core
## (rho ~= 1000 kg/m^3), which the terrain bake / collision / altitude use; the
## "radius" in _body_frames stays the reference (cloud tops). == the reference
## radius for every other body.
var _body_surface_radius: Dictionary = {}
## The body id whose centre is the canonical (0,0,0) -- i.e. the active body.
var active_body_id: StringName = &"asterra"

## --- Local physics frame ------------------------------------------------------
var origin: Vec3D = Vec3D.new(0, 0, 0)

var _rebase_count: int = 0
var _pending_rebase_origin: Vec3D


func _ready() -> void:
	# Ordinary render/process consumers should still see Frames early, but the
	# stronger guarantee is physics ordering: a queued rebase is committed before
	# player, ragdoll and vehicle physics update for that tick.
	process_priority = -100
	process_physics_priority = REBASE_PHYSICS_PRIORITY


func _physics_process(_dt: float) -> void:
	_commit_pending_rebase()


## The single advancer of simulated time. Consumers read `system_time_s`; the
## Planet Studio panel scrubs it directly; nothing else mutates it.
func _process(dt: float) -> void:
	if playing:
		system_time_s += dt * time_scale


func set_planet_radius(r: float) -> void:
	planet_radius = r


## Exact angular radius of Helion as seen from Asterra. For the default physical
## geometry this is about 0.2667 degrees (0.5334 degree apparent diameter).
func helion_angular_radius_rad() -> float:
	var ratio := clampf(helion_radius_m / maxf(helion_distance_m, 1.0), 0.0, 0.999999)
	return asin(ratio)


func helion_angular_diameter_deg() -> float:
	return rad_to_deg(helion_angular_radius_rad() * 2.0)


## Asterra frame (double) -> local render frame (float32).
func to_render(p: Vec3D) -> Vector3:
	return Vector3(float(p.x - origin.x), float(p.y - origin.y), float(p.z - origin.z))


## Local render frame -> Asterra frame.
func to_world(p: Vector3) -> Vec3D:
	return Vec3D.new(origin.x + p.x, origin.y + p.y, origin.z + p.z)


## Surface point helpers -------------------------------------------------------
func dir_altitude_to_world(d: Vector3, altitude: float) -> Vec3D:
	var r := planet_radius + altitude
	return Vec3D.new(d.x * r, d.y * r, d.z * r)


func world_to_dir(p: Vec3D) -> Vector3:
	return p.normalized().to_v3()


func world_altitude(p: Vec3D) -> float:
	return p.length() - planet_radius


## --- Per-body sub-frame helpers (multi-planet transition) -------------------
## Record body `id`'s centre (system frame) and radius. `center` for the active
## body is the zero vector.
func set_body_frame(id: StringName, center_system: Vec3D, radius: float,
		surface_radius: float = -1.0) -> void:
	_body_frames[id] = {"center": center_system.dup(), "radius": maxf(radius, 1.0)}
	_body_surface_radius[id] = maxf(surface_radius, 1.0) if surface_radius > 0.0 \
		else maxf(radius, 1.0)


func has_body_frame(id: StringName) -> bool:
	return _body_frames.has(id)


## Centre of body `id` in the system frame (root star at origin). Zero if unknown.
func body_center_system(id: StringName) -> Vec3D:
	var f: Dictionary = _body_frames.get(id, {})
	var c: Vec3D = f.get("center") as Vec3D
	return c.dup() if c != null else Vec3D.new()


func body_radius(id: StringName) -> float:
	var f: Dictionary = _body_frames.get(id, {})
	return float(f.get("radius", planet_radius))


## Surface datum radius of body `id` (its gas-giant core, else its reference
## radius). Falls back to the reference radius when unknown.
func body_surface_radius(id: StringName) -> float:
	return float(_body_surface_radius.get(id, body_radius(id)))


## Centre of body `id` expressed in the CURRENT canonical frame (active body at
## the origin): body_center_system(id) - body_center_system(active).
func body_center_canonical(id: StringName) -> Vec3D:
	if id == active_body_id or not _body_frames.has(id):
		return Vec3D.new()
	return body_center_system(id).sub(body_center_system(active_body_id))


## Altitude of canonical point `p` above body `id`'s reference sphere. For the
## active body this is `world_altitude(p)`; for another body it accounts for that
## body's offset centre + radius, so descent/approach logic can compare "height
## over A" against "height over B" during a swap.
func world_altitude_over(p: Vec3D, id: StringName) -> float:
	if id == active_body_id or not _body_frames.has(id):
		return world_altitude(p)
	return p.sub(body_center_canonical(id)).length() - body_radius(id)


## Re-origin the canonical frame onto body `id`: its centre becomes (0,0,0), the
## radius datum becomes its radius, and the render frame is rebased around
## `player_world_about_body` (the observer already re-expressed about `id`'s
## centre). This is the single "deep-space rebase" of an interplanetary cruise
## (M6) and also the near-body swap (M3/M5) -- the offset is just larger in the
## former. `_body_frames` centres are system-absolute so they need no adjustment.
func rebase_to_body(id: StringName, player_world_about_body: Vec3D) -> void:
	active_body_id = id
	if _body_frames.has(id):
		# The resident terrain stack is baked at the surface datum (a gas giant's
		# core), so the active radius datum must match it, not the cloud tops.
		planet_radius = body_surface_radius(id)
	rebase(player_world_about_body)


## Re-base the local frame onto a new Asterra-frame origin, and report the shift
## so live nodes can translate themselves. Explicit rebases (spawn/teleport/setup)
## remain immediate; any queued travel rebase is superseded by this exact request.
func rebase(new_origin: Vec3D) -> void:
	_pending_rebase_origin = null
	_commit_rebase(new_origin)


func _commit_rebase(new_origin: Vec3D) -> void:
	var delta := Vector3(
		float(origin.x - new_origin.x),
		float(origin.y - new_origin.y),
		float(origin.z - new_origin.z))
	origin = new_origin.dup()
	_rebase_count += 1
	origin_shifted.emit(delta)


## Called by moving observers with their current render-space position. Crossing
## the threshold requests a rebase; it intentionally does NOT mutate `origin` in
## the caller's callback. Returning true means a rebase is pending for the next
## physics-frame boundary.
func maintain_origin(observer_render_pos: Vector3) -> bool:
	if observer_render_pos.length() <= REBASE_THRESHOLD:
		return false
	# Convert while the current origin is still authoritative. If several observers
	# request in one physics tick, the latest request wins and is still committed
	# exactly once by the next early Frames physics callback.
	_pending_rebase_origin = to_world(observer_render_pos)
	return true


func _commit_pending_rebase() -> void:
	if _pending_rebase_origin == null:
		return
	var target: Vec3D = _pending_rebase_origin.dup()
	_pending_rebase_origin = null
	_commit_rebase(target)


func rebase_pending() -> bool:
	return _pending_rebase_origin != null


func rebase_count() -> int:
	return _rebase_count


## Direction from an Asterra point toward Helion, in the local render axes.
## helion_dir is stored as the sunward direction and is the convention used by
## the planet/cloud shaders.
func sun_dir_at(_d: Vector3) -> Vector3:
	return helion_dir


## Ecliptic sunward vector -> sunward direction in the anchor body's rotating,
## axially-tilted frame (i.e. what the terrain/cloud/sky shaders consume as
## `helion_dir`). `sunward_system` is the unit vector from the anchor body centre
## toward its root star, in the system frame (+Y = ecliptic north, orbit plane XZ).
##
## Conventions (all deliberate; axial_tilt_deg was previously unused at runtime):
##  - obliquity: right-handed rotation about system +X by axial_tilt_deg, so the
##    node line is system X (equinox at the system-X crossing, solstice at
##    system-Z), sub-solar latitude sweeps ±axial_tilt_deg over a year.
##  - diurnal spin: about the body's local +Y, signed so sub-solar LONGITUDE
##    increases with the clock -- this reproduces the shipped phase20 time-of-day
##    slider (dir_to_latlon(helion_dir) == (declination, hour), hour rising), which
##    is the convention the terrain shaders already assume.
func sun_dir_from_system(sunward_system: Vector3) -> Vector3:
	var s := sunward_system.normalized()
	if not s.is_normalized():
		return helion_dir
	var tilt := deg_to_rad(axial_tilt_deg)
	var spin := TAU * (system_time_s / maxf(day_seconds, 1e-6)) + deg_to_rad(_rotation_phase0_deg)
	var seasonal: Vector3 = Basis(Vector3(1.0, 0.0, 0.0), -tilt) * s
	return (Basis(Vector3(0.0, 1.0, 0.0), -spin) * seasonal).normalized()


## Inverse-square solar-brightness factor relative to REFERENCE_HELION_DISTANCE_M.
## Both the Godot sun light_energy and GraphicsQuality.solar_irradiance() scale by
## this so the surface and the scattering never disagree. Clamped so an extreme
## authored eccentricity cannot blow out eye adaptation.
func solar_distance_scale() -> float:
	var ratio := REFERENCE_HELION_DISTANCE_M / maxf(helion_distance_m, 1.0)
	return clampf(ratio * ratio, 0.1, 10.0)


## Sub-solar latitude (rad). Diurnal-invariant because the spin is about local +Y.
func sub_solar_latitude_rad() -> float:
	return asin(clampf(helion_dir.y, -1.0, 1.0))


## Length of one Asterra year in seconds. When an orbital period has been supplied
## (OrbitalMotionRuntime / main.gd), that is authoritative; otherwise fall back to
## the standalone year_days * day_seconds calendar.
var _orbit_period_s: float = 0.0

func set_orbit_period_s(value: float) -> void:
	_orbit_period_s = maxf(value, 0.0)
	if _orbit_period_s > 0.0 and day_seconds > 1e-6:
		year_days = _orbit_period_s / day_seconds

func year_seconds() -> float:
	if _orbit_period_s > 0.0:
		return _orbit_period_s
	return maxf(year_days * day_seconds, 1.0)


## Fractional day within the current Asterra year (0 .. year_days).
func day_of_year() -> float:
	return fposmod(system_time_s / maxf(day_seconds, 1e-6), maxf(year_days, 1e-6))
