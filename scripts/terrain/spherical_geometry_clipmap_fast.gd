extends "res://scripts/terrain/spherical_geometry_clipmap.gd"
## Playability-oriented performance pass for the 400-cell spherical clipmap.
##
## The 4K geometry target stays intact. Runtime visual streaming is deliberately
## bounded: sparse refinement is requested only through L6 (~48 m spacing); L7+
## uses the orbit elevation immediately. Broad visual requests are disk/RAM-only
## in GroundHeightStore, so camera movement cannot start an unbounded CPU bake.

const FAST_HORIZON_MARGIN_M: float = 2500.0
const FAST_REQUEST_GRID_STEPS: int = 9
const MIN_VISIBLE_REQUEST_INTERVAL_MS: int = 400
const REQUEST_INNER_SKIP_Q: float = 0.42
const SPARSE_VISUAL_MAX_LEVEL: int = 6
const FAST_MATERIAL_RES: float = 64.0
const FAST_MATERIAL_TEXEL_M := Vector3(16.0, 256.0, 4096.0)

var _last_visible_request_msec: int = -1000000


func _ready() -> void:
	super._ready()
	_material.shader = load("res://shaders/spherical_geometry_clipmap_fast.gdshader")

	# Until a cheap dedicated terrain-shadow representation exists, do not run the
	# expensive displaced 400-cell clipmap through directional shadow cascades.
	# This affects shadows only; terrain lighting/normals remain enabled.
	if _center_batch != null:
		_center_batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	if _ring_batch != null:
		_ring_batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF


func _update_visible_cap(observer_radius: float, planet_radius: float) -> void:
	var safe_r: float = maxf(observer_radius, planet_radius + 0.01)
	var horizon_angle: float = acos(clampf(planet_radius / safe_r, -1.0, 1.0))
	var horizon_arc: float = horizon_angle * planet_radius
	_visible_cap_arc_m = minf(PI * 0.5 * planet_radius,
		horizon_arc + FAST_HORIZON_MARGIN_M)


func _request_visible_pages() -> void:
	if not _have_anchor or Planet.cfg == null:
		return
	var now: int = Time.get_ticks_msec()
	if now - _last_visible_request_msec < MIN_VISIBLE_REQUEST_INTERVAL_MS:
		return
	_last_visible_request_msec = now

	# Do not ask the sparse cache to cover the entire horizon. L7+ is intentionally
	# handled by the orbit macro texture in the shader. Request one parent beyond
	# the active fine set only when it still lies inside the sparse band.
	var request_max: int = mini(_active_max_level + 1, SPARSE_VISUAL_MAX_LEVEL)
	for level: int in range(0, request_max + 1):
		var directions: Array[Vector3] = _request_directions_for_level(level)
		if directions.is_empty():
			continue
		var priority: float = REQUEST_PRIORITY_BASE + float(level) * REQUEST_PRIORITY_LEVEL_STEP
		GroundHeightStore.request_samples(directions, level, priority)
		GroundHeightPageAtlas.touch_samples(directions, level)


func _request_directions_for_level(level: int) -> Array[Vector3]:
	var result: Array[Vector3] = []
	if level > SPARSE_VISUAL_MAX_LEVEL:
		return result
	var spacing: float = _base_spacing * pow(2.0, float(level))
	var level_half: float = float(HALF_CELLS) * spacing
	var half: float = minf(level_half * 1.015, _visible_cap_arc_m * 1.04)
	var radial_limit: float = minf(level_half * 1.43, _visible_cap_arc_m * 1.04)
	var denom: float = float(FAST_REQUEST_GRID_STEPS - 1)
	for yi: int in FAST_REQUEST_GRID_STEPS:
		var fy: float = -1.0 + 2.0 * float(yi) / denom
		for xi: int in FAST_REQUEST_GRID_STEPS:
			var fx: float = -1.0 + 2.0 * float(xi) / denom
			# L1+ are annuli; don't probe the centre already represented by finer LODs.
			if level > 0 and maxf(absf(fx), absf(fy)) < REQUEST_INNER_SKIP_Q:
				continue
			var offset := Vector2(fx, fy) * half
			if offset.length() > radial_limit:
				continue
			result.append(_direction_for_offset(_center_dir, _center_right, _center_up,
				offset, Planet.cfg.planet_radius))
	return result


func _sync_material_control() -> void:
	var source: Node = get_node_or_null("/root/MaterialClipmap")
	if source == null:
		_material.set_shader_parameter("u_material_clipmap_ready", 0.0)
		return
	var value: Variant = source.get("_texture")
	if not (value is Texture2DArray):
		_material.set_shader_parameter("u_material_clipmap_ready", 0.0)
		return
	var texture: Texture2DArray = value
	if texture == _last_material_control:
		return
	_last_material_control = texture
	_material.set_shader_parameter("u_material_clipmap", texture)
	_material.set_shader_parameter("u_material_clipmap_ready", 1.0)
	_material.set_shader_parameter("u_material_center", source.get("_center"))
	_material.set_shader_parameter("u_material_right", source.get("_right"))
	_material.set_shader_parameter("u_material_up", source.get("_up"))
	_material.set_shader_parameter("u_material_res", FAST_MATERIAL_RES)
	_material.set_shader_parameter("u_material_texel_m", FAST_MATERIAL_TEXEL_M)


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["fast_path"] = true
	out["request_grid"] = FAST_REQUEST_GRID_STEPS
	out["far_shadow_pass"] = false
	out["sparse_visual_max_level"] = SPARSE_VISUAL_MAX_LEVEL
	return out
