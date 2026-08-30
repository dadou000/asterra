extends "res://scripts/terrain/spherical_geometry_clipmap_authoritative.gd"
## Production terrain wrapper for Planet Studio's BLANK backend.
##
## PROCEDURAL delegates unchanged to the authoritative cached renderer. BLANK keeps
## the exact spherical clipmap topology/camera tracking but suspends every generated
## height cache and switches to a shader-authored surface. Generated height and
## authored displacement are deliberately independent sources.

const BLANK_SHADER_PATH := "res://shaders/spherical_geometry_clipmap_blank_displacement.gdshader"
const PROCEDURAL_SHADER_PATH := "res://shaders/spherical_geometry_clipmap_cached_surface.gdshader"
const DISPLACEMENT_RUNTIME_SCRIPT := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime.gd")

var _blank_cache_state_applied: bool = false
var _blank_shader_active: bool = false
var _displacement_runtime: Node
var _displacement_fingerprint: String = ""


func _ready() -> void:
	super._ready()
	_ensure_displacement_runtime()
	_ensure_backend_shader(_blank_backend())
	_sync_authoring_displacement(true)


func _process(dt: float) -> void:
	var blank := _blank_backend()
	_ensure_backend_shader(blank)
	_set_cache_nodes_processing(not blank)
	_sync_authoring_displacement(false)
	super._process(dt)
	if blank:
		_apply_blank_material_state()
		# The inherited visibility gate normally requires a generated orbit/page
		# source. Blank has no such source, but its shader-displaced sphere is valid.
		_set_visible(true)
		_blank_cache_state_applied = true
	elif _blank_cache_state_applied:
		_blank_cache_state_applied = false
		_force_cache_rebind()


func _ensure_displacement_runtime() -> void:
	if _displacement_runtime != null and is_instance_valid(_displacement_runtime):
		return
	_displacement_runtime = DISPLACEMENT_RUNTIME_SCRIPT.new() as Node
	if _displacement_runtime == null:
		return
	_displacement_runtime.name = "TerrainDisplacementRuntime"
	add_child(_displacement_runtime)


func _sync_authoring_displacement(force: bool) -> void:
	_ensure_displacement_runtime()
	if _displacement_runtime == null:
		return
	var editor: Node = get_tree().root.find_child("PlanetStudioLive", true, false)
	if editor != null:
		var session_value: Variant = editor.get("_session")
		if session_value != null and session_value.has_method("active_terrain_profile"):
			var terrain: Resource = session_value.call("active_terrain_profile") as Resource
			var fingerprint: String = String(
				_displacement_runtime.call("profile_fingerprint", terrain))
			if force or fingerprint != _displacement_fingerprint:
				_displacement_fingerprint = fingerprint
				_displacement_runtime.call("compile_from_terrain", terrain)
	if _material != null and _displacement_runtime.has_method("bind_material"):
		_displacement_runtime.call("bind_material", _material)


func _ensure_backend_shader(blank: bool) -> void:
	if _material == null or blank == _blank_shader_active:
		return
	var path: String = BLANK_SHADER_PATH if blank else PROCEDURAL_SHADER_PATH
	if not ResourceLoader.exists(path, "Shader"):
		push_error("Terrain backend shader is missing: %s" % path)
		return
	var shader: Shader = load(path) as Shader
	if shader == null:
		push_error("Terrain backend shader failed to load: %s" % path)
		return
	_material.shader = shader
	_blank_shader_active = blank
	if blank:
		_apply_blank_material_state()
		_sync_authoring_displacement(true)
	else:
		# Shader replacement clears every binding. Restore the exact production
		# resources before allowing the inherited cache stack to run again.
		super._bind_gpu_resources(true)
		super._sync_material_control()
		_bind_gpu_context(true)
		_bind_surface_pbr(true)
		_sync_detail_seed()
		_sync_debug_uniforms()
		_force_cache_rebind()


func _bind_gpu_resources(force: bool) -> void:
	if _blank_backend() and _material != null:
		_material.set_shader_parameter("u_macro_ready", 0.0)
		_material.set_shader_parameter("u_page_atlas_ready", 0.0)
		_material.set_shader_parameter("u_orbit_ready", 0.0)
		return
	super._bind_gpu_resources(force)


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
		_sync_authoring_displacement(false)


func _apply_blank_material_state() -> void:
	if _material == null:
		return
	# `u_height_enabled` is retained only as a compatibility uniform for older
	# debug controls. The Blank shader does not use it to gate authored shape.
	_material.set_shader_parameter("u_height_enabled", 0.0)
	_material.set_shader_parameter("u_generated_height_enabled", 0.0)
	_material.set_shader_parameter("u_shader_displacement_enabled", 1.0)
	_material.set_shader_parameter("u_macro_ready", 0.0)
	_material.set_shader_parameter("u_page_atlas_ready", 0.0)
	_material.set_shader_parameter("u_orbit_ready", 0.0)
	_material.set_shader_parameter("u_terrain_cache_ready", 0.0)
	_material.set_shader_parameter("u_material_clipmap_ready", 0.0)
	_material.set_shader_parameter("u_material_global_ready", 0.0)
	if _displacement_runtime != null and _displacement_runtime.has_method("bind_material"):
		_displacement_runtime.call("bind_material", _material)


func _set_cache_nodes_processing(enabled: bool) -> void:
	for cache: Node in [_terrain_cache_active, _terrain_cache_staging]:
		if cache == null or not is_instance_valid(cache):
			continue
		cache.set_process(enabled)
		cache.set_physics_process(enabled)


func set_heightmap_enabled(value: bool) -> void:
	# External debug controls may toggle generated height in Procedural mode. Blank
	# always has generated height disabled, but authored shader displacement remains.
	super.set_heightmap_enabled(value and not _blank_backend())
	if _blank_backend() and _material != null:
		_material.set_shader_parameter("u_generated_height_enabled", 0.0)
		_material.set_shader_parameter("u_shader_displacement_enabled", 1.0)


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["blank_analytic_base"] = _blank_backend()
	out["generated_heightmap"] = not _blank_backend()
	out["generated_material_map"] = not _blank_backend()
	out["shader_displacement_enabled"] = _blank_backend()
	if _displacement_runtime != null and _displacement_runtime.has_method("stats"):
		out["author_displacement"] = _displacement_runtime.call("stats")
	if _blank_backend():
		out["terrain_cache_active"] = false
		out["terrain_cache_ready"] = false
		out["coverage_ready"] = true
	return out


func _blank_backend() -> bool:
	return bool(Planet.get("blank_mode"))
