extends Node
## Autoload: hierarchical reference frames + floating origin.
##
##   Helion barycentric frame -> Asterra body-fixed frame -> regional frame
##                                                     -> local physics frame
##                                                     -> assembly frame
##
## CelestialSystem owns the double-precision N-body state. This singleton keeps
## the existing Asterra-centred/body-fixed API used by terrain, rendering and
## gameplay, then converts to the local float32 physics frame near the observer.

signal origin_shifted(delta_render: Vector3)

const REBASE_THRESHOLD := 4096.0

## --- Helion/Asterra compatibility state --------------------------------------
## Existing renderers read these fields directly, so keep them synchronized with
## the authoritative CelestialSystem every frame.
var helion_dir: Vector3 = Vector3(-1.0, 0.0, 0.0)
var axial_tilt_deg: float = 26.0
var day_seconds: float = 11.5 * 3600.0
var year_days: float = 644.0629663009096

## --- Asterra ------------------------------------------------------------------
var planet_radius: float = 3500000.0

## --- Local physics frame ------------------------------------------------------
var origin: Vec3D = Vec3D.new(0, 0, 0)

var _rebase_count: int = 0

func _ready() -> void:
	process_priority = -100
	_sync_celestial_state()

func _process(_delta: float) -> void:
	_sync_celestial_state()

func _sync_celestial_state() -> void:
	helion_dir = CelestialSystem.sun_dir_body()
	axial_tilt_deg = CelestialSystem.ASTERRA_AXIAL_TILT_DEG
	day_seconds = CelestialSystem.ASTERRA_ROTATION_PERIOD_S
	year_days = CelestialSystem.orbital_period_asterra_days()
	planet_radius = CelestialSystem.ASTERRA_RADIUS_M

func set_planet_radius(r: float) -> void:
	# Terrain/config callers still use this setter. The canonical celestial radius
	# is locked at 3500 km; warn through the value by snapping back on next frame
	# rather than allowing reference frames and gravity to silently disagree.
	planet_radius = r

## Asterra body-fixed double -> local render frame (float32).
func to_render(p: Vec3D) -> Vector3:
	return Vector3(float(p.x - origin.x), float(p.y - origin.y), float(p.z - origin.z))

## Local render frame -> Asterra body-fixed double.
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
## so live nodes can translate themselves.
func rebase(new_origin: Vec3D) -> void:
	var delta := Vector3(
		float(origin.x - new_origin.x),
		float(origin.y - new_origin.y),
		float(origin.z - new_origin.z))
	origin = new_origin.dup()
	_rebase_count += 1
	origin_shifted.emit(delta)

## Call each frame with the observer's render-space position.
func maintain_origin(observer_render_pos: Vector3) -> bool:
	if observer_render_pos.length() > REBASE_THRESHOLD:
		rebase(to_world(observer_render_pos))
		return true
	return false

func rebase_count() -> int:
	return _rebase_count

## Direction from any Asterra surface point toward Helion. At ~0.885 AU the
## parallax across a 7000 km planet is negligible for the directional-light path,
## so the body-fixed direction is shared across the surface.
func sun_dir_at(_d: Vector3) -> Vector3:
	return CelestialSystem.sun_dir_body()
