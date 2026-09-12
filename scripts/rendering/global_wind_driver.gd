extends Node
## Pushes the shared scatter wind field (u_global_wind_direction/speed/strength,
## declared in shaders/gpu_scatter_common.gdshaderinc) to every material that
## reads it, on the same throttled cadence VolumetricCloudController uses for
## its own per-frame shader-global pushes.
##
## Direction/speed default to a fixed constant today (matching the existing
## (11, 0, 4.5) m/s cosmetic wind used for cloud/rain drift, so scatter sway
## agrees visually with cloud/particle drift direction). A future pass can
## drive this from live weather data once weather_system.gd exposes an actual
## wind vector; none exists there today.

const UPDATE_INTERVAL := 0.10
const DEFAULT_DIRECTION := Vector2(0.9258, 0.3785)
const DEFAULT_SPEED := 1.6
const DEFAULT_STRENGTH := 1.0

var _accum := 0.0


func _ready() -> void:
	_push()


func _process(delta: float) -> void:
	_accum += delta
	if _accum < UPDATE_INTERVAL:
		return
	_accum = 0.0
	_push()


func _push() -> void:
	RenderingServer.global_shader_parameter_set("u_global_wind_direction", DEFAULT_DIRECTION)
	RenderingServer.global_shader_parameter_set("u_global_wind_speed", DEFAULT_SPEED)
	RenderingServer.global_shader_parameter_set("u_global_wind_strength", DEFAULT_STRENGTH)
