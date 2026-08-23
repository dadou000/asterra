extends "res://scripts/terrain/spherical_geometry_clipmap_fast.gd"
## Hybrid spherical clipmap.
##
## L0-L3 are generated directly in the vertex shader from the macro/orbit parent,
## so they never wait for sparse height pages and cannot appear as rectangular
## streaming blocks. L4-L6 remain sparse authoritative transition levels, and L7+
## uses the orbit field directly.

const GPU_PROCEDURAL_MAX_LEVEL: int = 3
const SPARSE_VISUAL_MIN_LEVEL: int = 4
const GPU_DETAIL_STRENGTH: float = 1.0


func _ready() -> void:
	super._ready()
	_material.shader = load("res://shaders/spherical_geometry_clipmap_gpu_detail.gdshader")
	_bind_gpu_resources(true)
	_material.set_shader_parameter("u_page_fade_seconds", PAGE_REFINEMENT_FADE_SECONDS)
	_sync_gpu_detail_uniforms()
	Planet.world_ready.connect(_on_gpu_detail_world_ready)


func _on_gpu_detail_world_ready(_fields: PlanetFields) -> void:
	_sync_gpu_detail_uniforms()


func _sync_gpu_detail_uniforms() -> void:
	if _material == null or Planet.cfg == null:
		return
	var gpu_seed: int = Planet.cfg.stream_seed("gpu_visual_detail")
	var sx: float = float(gpu_seed & 0xffff) / 65535.0 * TAU
	var sy: float = float((gpu_seed >> 16) & 0xffff) / 65535.0 * TAU
	var sz: float = float((gpu_seed >> 32) & 0xffff) / 65535.0 * TAU
	_material.set_shader_parameter("u_gpu_detail_seed", Vector3(sx, sy, sz))
	_material.set_shader_parameter("u_gpu_detail_strength",
		GPU_DETAIL_STRENGTH * maxf(0.05, Planet.cfg.detail_amplitude / 260.0))


func _request_visible_pages() -> void:
	if not _have_anchor or Planet.cfg == null:
		return
	var now: int = Time.get_ticks_msec()
	if now - _last_visible_request_msec < MIN_VISIBLE_REQUEST_INTERVAL_MS:
		return
	_last_visible_request_msec = now

	# L0-L3 are pure GPU procedural refinement and deliberately never enter the
	# sparse page request/cache path. Only L4-L6 need disk/RAM-backed pages.
	var request_max: int = mini(_active_max_level + 1, SPARSE_VISUAL_MAX_LEVEL)
	if request_max < SPARSE_VISUAL_MIN_LEVEL:
		return
	for level: int in range(SPARSE_VISUAL_MIN_LEVEL, request_max + 1):
		var directions: Array[Vector3] = super._request_directions_for_level(level)
		if directions.is_empty():
			continue
		var base_priority: float = REQUEST_PRIORITY_BASE + float(level) * REQUEST_PRIORITY_LEVEL_STEP
		var level_half: float = maxf(float(HALF_CELLS) * _base_spacing
			* pow(2.0, float(level)), 1.0)
		var priorities := PackedFloat32Array()
		priorities.resize(directions.size())
		for i: int in directions.size():
			var angular_distance: float = acos(clampf(_center_dir.dot(directions[i]), -1.0, 1.0))
			var distance_m: float = angular_distance * Planet.cfg.planet_radius
			var radial_q: float = clampf(distance_m / level_half, 0.0, 1.0)
			priorities[i] = base_priority + radial_q * REQUEST_RADIAL_PRIORITY_SPAN

		if GroundHeightStore.has_method("request_samples_prioritized"):
			GroundHeightStore.request_samples_prioritized(directions, level, priorities)
		else:
			GroundHeightStore.request_samples(directions, level, base_priority)
		GroundHeightPageAtlas.touch_samples(directions, level)


func _request_directions_for_level(level: int) -> Array[Vector3]:
	if level < SPARSE_VISUAL_MIN_LEVEL:
		return []
	return super._request_directions_for_level(level)


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["gpu_procedural"] = true
	out["gpu_procedural_max_level"] = GPU_PROCEDURAL_MAX_LEVEL
	out["sparse_visual_min_level"] = SPARSE_VISUAL_MIN_LEVEL
	return out
