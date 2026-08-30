extends "res://scripts/terrain/spherical_geometry_clipmap_authoritative.gd"
## Production terrain wrapper for Planet Studio's BLANK backend.
##
## PROCEDURAL delegates unchanged to the authoritative cached renderer. BLANK keeps
## the exact same spherical clipmap topology/camera tracking but suspends every
## generated-height cache and page request. The ground shader therefore receives an
## analytic radius-only sphere and can be replaced/overridden by authoring shaders.

var _blank_cache_state_applied: bool = false


func _process(dt: float) -> void:
	var blank := _blank_backend()
	_set_cache_nodes_processing(not blank)
	super._process(dt)
	if blank:
		_apply_blank_material_state()
		# The inherited visibility gate normally requires a generated orbit/page
		# height source. Blank has neither, yet its analytic sphere is valid geometry.
		_set_visible(true)
		_blank_cache_state_applied = true
	elif _blank_cache_state_applied:
		_blank_cache_state_applied = false
		_force_cache_rebind()


func _request_visible_pages() -> void:
	if _blank_backend():
		return
	super._request_visible_pages()


func _plan_handoff(plane_offset: Vector2) -> void:
	if _blank_backend():
		return
	super._plan_handoff(plane_offset)


func _update_terrain_caches() -> void:
	if _blank_backend():
		_bound_cache_ready = false
		if _material != null:
			_material.set_shader_parameter("u_terrain_cache_ready", 0.0)
		return
	super._update_terrain_caches()


func _sync_material_control() -> void:
	if _blank_backend():
		if _material != null:
			# No procedural material/biome field exists on a Blank body. Manual biome
			# authoring remains a separate sparse categorical overlay.
			_material.set_shader_parameter("u_material_clipmap_ready", 0.0)
			_material.set_shader_parameter("u_material_global_ready", 0.0)
		_last_material_control = null
		return
	super._sync_material_control()


func _sync_uniforms(origin: Vector3) -> void:
	super._sync_uniforms(origin)
	if _blank_backend():
		_apply_blank_material_state()


func _apply_blank_material_state() -> void:
	if _material == null:
		return
	_material.set_shader_parameter("u_height_enabled", 0.0)
	_material.set_shader_parameter("u_page_atlas_ready", 0.0)
	_material.set_shader_parameter("u_orbit_ready", 0.0)
	_material.set_shader_parameter("u_terrain_cache_ready", 0.0)
	_material.set_shader_parameter("u_material_clipmap_ready", 0.0)
	_material.set_shader_parameter("u_material_global_ready", 0.0)


func _set_cache_nodes_processing(enabled: bool) -> void:
	for cache: Node in [_terrain_cache_active, _terrain_cache_staging]:
		if cache == null or not is_instance_valid(cache):
			continue
		cache.set_process(enabled)
		cache.set_physics_process(enabled)


func set_heightmap_enabled(value: bool) -> void:
	# External debug controls may still toggle generated height in Procedural mode,
	# but Blank always wins because it has no heightmap resource to enable.
	super.set_heightmap_enabled(value and not _blank_backend())


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["blank_analytic"] = _blank_backend()
	out["generated_heightmap"] = not _blank_backend()
	out["generated_material_map"] = not _blank_backend()
	if _blank_backend():
		out["terrain_cache_active"] = false
		out["terrain_cache_ready"] = false
		out["coverage_ready"] = true
	return out


func _blank_backend() -> bool:
	return bool(Planet.get("blank_mode"))
