extends Node
## GPU-visual terrain does not prefetch CPU height pages.
##
## TerrainCollisionStreamer owns its own motion-biased source requests, so the old
## camera visual prefetcher would only duplicate CPU baking after visual terrain
## moved entirely to the GPU.

var _last_speed_mps: float = 0.0


func _ready() -> void:
	process_priority = 6


func _process(_dt: float) -> void:
	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_last_speed_mps = 0.0
		return
	# Kept intentionally as a zero-cost status shim for the existing HUD.
	_last_speed_mps = 0.0


func stats() -> Dictionary:
	return {
		"speed_mps": _last_speed_mps,
		"lookahead_m": 0.0,
		"visual_prefetch": false,
		"gpu_visual": true,
	}
