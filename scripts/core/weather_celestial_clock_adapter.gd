extends Node
## Compatibility facade for the water/weather stack after the celestial runtime
## moved to Frames + the authored orbital system. Keep the legacy autoload name
## `CelestialSystem` so weather code can consume one stable clock/solar contract
## without reintroducing the obsolete standalone N-body runtime.

signal state_updated(simulation_seconds: float)

const ASTERRA_MEAN_IRRADIANCE_W_M2 := 1420.0

var simulation_seconds: float:
	get:
		return Frames.system_time_s

var time_scale: float:
	get:
		return Frames.time_scale
	set(value):
		Frames.time_scale = value

var _last_time_s := 0.0

func _ready() -> void:
	# Frames advances at priority -100. Run after it but before WeatherSystem's
	# default process priority so state_updated and simulation_seconds agree.
	process_priority = -90
	_last_time_s = Frames.system_time_s

func _process(_delta: float) -> void:
	var now := Frames.system_time_s
	if not is_equal_approx(now, _last_time_s):
		_last_time_s = now
		state_updated.emit(now)

func set_time_scale(value: float) -> void:
	Frames.time_scale = value

func sun_dir_body() -> Vector3:
	return Frames.helion_dir.normalized()

func asterra_irradiance_w_m2() -> float:
	return ASTERRA_MEAN_IRRADIANCE_W_M2 * Frames.solar_distance_scale()

func helion_angular_diameter_deg() -> float:
	return Frames.helion_angular_diameter_deg()
