extends "res://scripts/terrain/spherical_geometry_clipmap.gd"
## Performance pass for the 400-cell spherical clipmap.
##
## Keeps the 4K topology target intact while reducing the two costs that were
## dominating the first run: dense GDScript page-probe sweeps and repeated far
## terrain shadow passes. The GPU shader also uses the one-hash-per-page fast path.

const FAST_HORIZON_MARGIN_M: float = 3000.0
const FAST_REQUEST_GRID_STEPS: int = 17
const MIN_VISIBLE_REQUEST_INTERVAL_MS: int = 180
const REQUEST_INNER_SKIP_Q: float = 0.40

var _last_visible_request_msec: int = -1000000


func _ready() -> void:
	super._ready()
	_material.shader = load("res://shaders/spherical_geometry_clipmap_fast.gdshader")
	# One MultiMesh contains every outer level. Rendering that same million-vertex
	# batch through several directional shadow cascades multiplies vertex cost.
	# Keep only the L0 centre in the normal shadow pass for now; a dedicated coarse
	# terrain-shadow clipmap can restore long-range self-shadow independently.
	if _ring_batch != null:
		_ring_batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF


func _update_visible_cap(observer_radius: float, planet_radius: float) -> void:
	var safe_r: float = maxf(observer_radius, planet_radius + 0.01)
	var horizon_angle: float = acos(clampf(planet_radius / safe_r, -1.0, 1.0))
	var horizon_arc: float = horizon_angle * planet_radius
	_visible_cap_arc_m = minf(PI * 0.5 * planet_radius,
		horizon_arc + FAST_HORIZON_MARGIN_M)


func _request_visible_pages() -> void:
	var now: int = Time.get_ticks_msec()
	if now - _last_visible_request_msec < MIN_VISIBLE_REQUEST_INTERVAL_MS:
		return
	_last_visible_request_msec = now
	super._request_visible_pages()


func _request_directions_for_level(level: int) -> Array[Vector3]:
	var result: Array[Vector3] = []
	var spacing: float = _base_spacing * pow(2.0, float(level))
	var level_half: float = float(HALF_CELLS) * spacing
	var half: float = minf(level_half * 1.02, _visible_cap_arc_m * 1.08)
	var radial_limit: float = minf(level_half * 1.45, _visible_cap_arc_m * 1.08)
	var denom: float = float(FAST_REQUEST_GRID_STEPS - 1)
	for yi: int in FAST_REQUEST_GRID_STEPS:
		var fy: float = -1.0 + 2.0 * float(yi) / denom
		for xi: int in FAST_REQUEST_GRID_STEPS:
			var fx: float = -1.0 + 2.0 * float(xi) / denom
			# L1+ are annuli; do not repeatedly request the square hole already
			# covered by finer levels.
			if level > 0 and maxf(absf(fx), absf(fy)) < REQUEST_INNER_SKIP_Q:
				continue
			var offset := Vector2(fx, fy) * half
			if offset.length() > radial_limit:
				continue
			result.append(_direction_for_offset(_center_dir, _center_right, _center_up,
				offset, Planet.cfg.planet_radius))
	return result


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["fast_path"] = true
	out["request_grid"] = FAST_REQUEST_GRID_STEPS
	out["far_shadow_pass"] = false
	return out
