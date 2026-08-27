class_name OceanVisualInteractions
extends RefCounted
## Bounded visual disturbance history shared by every water quality preset.
##
## Gameplay physics must never depend on this buffer. It exists solely to let the
## renderer react to impacts and moving hulls without requiring a full volumetric
## fluid solve. Events are stored in planet-space directions, so floating-origin
## shifts and ocean clipmap re-anchors cannot move an existing wake.

const MAX_EVENTS := 64
const TEXTURE_ROWS := 3
const EVENT_IMPACT := 0.0
const EVENT_WAKE := 1.0

var _events: Array[Dictionary] = []
var _texture: ImageTexture
var _dirty := true


func texture() -> Texture2D:
	if _texture == null:
		_rebuild_texture()
	return _texture


func clear() -> void:
	_events.clear()
	_dirty = true


func prune(now_s: float, lifetime_s: float, budget: int) -> void:
	var keep: Array[Dictionary] = []
	for event: Dictionary in _events:
		if now_s - float(event["start_time"]) <= lifetime_s:
			keep.append(event)
	_events = keep
	while _events.size() > mini(maxi(budget, 0), MAX_EVENTS):
		_events.pop_front()
	_dirty = true


func add_impact(world_position: Vec3D, amplitude_m: float, radius_m: float,
		wavelength_m := 5.0, propagation_speed_mps := 8.0, foam := 0.35) -> void:
	_add_event(world_position, Vector3.ZERO, EVENT_IMPACT, amplitude_m, radius_m,
		wavelength_m, propagation_speed_mps, foam)


func add_wake(world_position: Vec3D, travel_direction: Vector3, amplitude_m: float,
		beam_m: float, wavelength_m := 12.0, propagation_speed_mps := 10.0,
		foam := 0.7) -> void:
	_add_event(world_position, travel_direction, EVENT_WAKE, amplitude_m,
		maxf(beam_m, 1.0), wavelength_m, propagation_speed_mps, foam)


func _add_event(world_position: Vec3D, travel_direction: Vector3, event_type: float,
		amplitude_m: float, radius_m: float, wavelength_m: float,
		propagation_speed_mps: float, foam: float) -> void:
	if world_position.length() <= 1.0:
		return
	var up := world_position.normalized().to_v3()
	var travel := travel_direction - up * travel_direction.dot(up)
	if travel.length_squared() < 1e-8:
		travel = CubeSphere.tangent_basis(up)[0]
	else:
		travel = travel.normalized()
	_events.append({
		"dir": up,
		"travel": travel,
		"type": event_type,
		"start_time": float(Time.get_ticks_usec()) / 1000000.0,
		"amplitude": clampf(amplitude_m, -50.0, 50.0),
		"radius": clampf(radius_m, 0.25, 1000.0),
		"wavelength": clampf(wavelength_m, 0.25, 1000.0),
		"speed": clampf(propagation_speed_mps, 0.1, 100.0),
		"foam": clampf(foam, 0.0, 1.0),
	})
	while _events.size() > MAX_EVENTS:
		_events.pop_front()
	_dirty = true


func sync_texture() -> Texture2D:
	if _dirty or _texture == null:
		_rebuild_texture()
	return _texture


func event_count() -> int:
	return _events.size()


func _rebuild_texture() -> void:
	var image := Image.create(MAX_EVENTS, TEXTURE_ROWS, false, Image.FORMAT_RGBAF)
	image.fill(Color(0.0, 0.0, 0.0, 0.0))
	for i in mini(_events.size(), MAX_EVENTS):
		var event: Dictionary = _events[i]
		var d: Vector3 = event["dir"]
		var travel: Vector3 = event["travel"]
		image.set_pixel(i, 0, Color(d.x, d.y, d.z, float(event["start_time"])))
		image.set_pixel(i, 1, Color(travel.x, travel.y, travel.z, float(event["type"])))
		image.set_pixel(i, 2, Color(float(event["amplitude"]), float(event["radius"]),
			float(event["wavelength"]), float(event["speed"])))
		# Foam is packed by sign-preserving multiplication into a fourth virtual
		# scalar: wake/impact amplitude is scaled in the shader and foam strength is
		# supplied globally by the quality profile. This keeps the texture at 3 rows.
	if _texture == null:
		_texture = ImageTexture.create_from_image(image)
	else:
		_texture.update(image)
	_dirty = false
