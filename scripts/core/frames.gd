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
var helion_dir: Vector3 = Vector3(1, 0.15, 0.3).normalized()  ## sunward, Asterra frame
## Physical source geometry. These defaults match the existing climate assumption
## of Sol-like irradiance at one astronomical unit. Rendering systems derive the
## apparent solar disc from these values instead of carrying unrelated blur angles.
var helion_radius_m: float = 696340000.0
var helion_distance_m: float = 149597870700.0
var axial_tilt_deg: float = 21.4
var day_seconds: float = 90000.0
var year_days: float = 402.0

## --- Asterra ------------------------------------------------------------------
var planet_radius: float = 1000000.0

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
