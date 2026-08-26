extends "res://scripts/terrain/spherical_geometry_clipmap_micro.gd"
## Final cached terrain-rendering layer.
##
## The existing micro/global GPU clipmap remains authoritative for topology, LOD,
## snapping, culling and debug controls. This thin layer only replaces the surface
## shader with the cache-aware variant and drives an incremental RenderingDevice
## cache from the exact same stable-anchor state.

const CACHED_SURFACE_SHADER_PATH := "res://shaders/spherical_geometry_clipmap_cached_surface.gdshader"
const TerrainClipmapCacheScript := preload("res://scripts/terrain/gpu_terrain_clipmap_cache.gd")

var _terrain_cache: Node
var _bound_cache_texture: Variant = null
var _bound_cache_generation := -1
var _bound_cache_res := -1
var _bound_cache_ready := false


func _ready() -> void:
	# Build the proven production renderer first. Replacing only the material here
	# leaves the entire ring/sector/micro topology unchanged and keeps rollback small.
	super._ready()
	_material.shader = load(CACHED_SURFACE_SHADER_PATH)

	# Shader replacement clears every parameter binding; restore the same resources
	# that global_gpu normally owns before enabling the cache.
	_bind_gpu_resources(true)
	_sync_material_control()
	_bind_gpu_context(true)
	_bind_surface_pbr(true)
	_sync_detail_seed()
	_sync_debug_uniforms()
	_material.set_shader_parameter("u_terrain_cache_ready", 0.0)

	_terrain_cache = TerrainClipmapCacheScript.new()
	_terrain_cache.name = "GPUTerrainClipmapCache"
	add_child(_terrain_cache)


func _process(dt: float) -> void:
	# The parent performs camera snapping, LOD selection, resource bindings and all
	# production visibility work. Cache synthesis is deliberately scheduled after
	# that state is final for this frame.
	super._process(dt)
	_update_terrain_cache()


func _update_terrain_cache() -> void:
	if _terrain_cache == null or not is_instance_valid(_terrain_cache) \
			or _material == null or Planet.cfg == null or not Planet.ready_state:
		return

	_terrain_cache.call("update_cache",
		_anchor_dir, _anchor_right, _anchor_up,
		_center_plane, _base_spacing,
		_active_min_level, _active_max_level)

	var texture: Variant = _terrain_cache.call("texture")
	if texture != null and texture != _bound_cache_texture:
		_bound_cache_texture = texture
		_material.set_shader_parameter("u_terrain_cache", texture)

	var ready := bool(_terrain_cache.call("cache_ready")) and texture != null
	if ready != _bound_cache_ready:
		_bound_cache_ready = ready
		_material.set_shader_parameter("u_terrain_cache_ready", 1.0 if ready else 0.0)

	var generation := int(_terrain_cache.call("anchor_generation"))
	if generation != _bound_cache_generation:
		_bound_cache_generation = generation
		_material.set_shader_parameter("u_terrain_cache_generation", generation)

	var cache_res := int(_terrain_cache.call("cache_resolution"))
	if cache_res != _bound_cache_res:
		_bound_cache_res = cache_res
		_material.set_shader_parameter("u_terrain_cache_res", cache_res)


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["terrain_cache_architecture"] = "toroidal_gpu_staggered"
	out["terrain_cache_active"] = _bound_cache_ready
	if _terrain_cache != null and is_instance_valid(_terrain_cache):
		var cache_stats: Dictionary = _terrain_cache.call("stats")
		out["terrain_cache_ready"] = bool(cache_stats.get("ready", false))
		out["terrain_cache_bindings_ready"] = bool(cache_stats.get("bindings_ready", false))
		out["terrain_cache_res"] = int(cache_stats.get("resolution", 0))
		out["terrain_cache_levels"] = int(cache_stats.get("levels", 0))
		out["terrain_cache_generation"] = int(cache_stats.get("generation", 0))
		out["terrain_cache_queued_jobs"] = int(cache_stats.get("queued_jobs", 0))
		out["terrain_cache_last_frame_jobs"] = int(cache_stats.get("last_frame_jobs", 0))
		out["terrain_cache_last_frame_samples"] = int(cache_stats.get("last_frame_samples", 0))
		out["terrain_cache_sample_budget"] = int(cache_stats.get("sample_budget", 0))
		out["terrain_cache_samples_total"] = int(cache_stats.get("samples_dispatched", 0))
		out["terrain_cache_strip_updates"] = int(cache_stats.get("strip_updates", 0))
		out["terrain_cache_anchor_resets"] = int(cache_stats.get("anchor_resets", 0))
		out["terrain_cache_toroidal"] = bool(cache_stats.get("toroidal", false))
		out["terrain_cache_staggered"] = bool(cache_stats.get("staggered", false))
	return out
