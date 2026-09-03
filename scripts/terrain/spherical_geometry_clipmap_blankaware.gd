extends "res://scripts/terrain/spherical_geometry_clipmap_authoritative.gd"
## Production terrain wrapper for Planet Studio's BLANK backend.
##
## PROCEDURAL delegates to the authoritative cached renderer while still allowing
## scoped authored shader graphs to compose on top. BLANK keeps the exact spherical
## clipmap topology/camera tracking but suspends every generated height/material
## cache and uses the authored displacement/material programs as its surface source.

const BLANK_SHADER_PATH := "res://shaders/spherical_geometry_clipmap_blank_displacement.gdshader"
const PROCEDURAL_SHADER_PATH := "res://shaders/spherical_geometry_clipmap_cached_surface.gdshader"
const DISPLACEMENT_RUNTIME_SCRIPT := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime.gd")
const MATERIAL_RUNTIME_SCRIPT := preload(
	"res://scripts/world_authoring/terrain_material_runtime.gd")

const AUTHORING_NONE_FINGERPRINT := "__no_enabled_authoring_graph__"
const AUTHORED_SHADER_DEFINE := "#define ASTERRA_AUTHORED_TERRAIN\n"
const AUTHORED_PROCEDURAL_INCLUDES := \
	"#include \"res://shaders/terrain_author_procedural_displacement.gdshaderinc\"\n" + \
	"#include \"res://shaders/terrain_author_material_post_cached.gdshaderinc\"\n"
const AUTHORED_BLANK_INCLUDES := \
	"#include \"res://shaders/terrain_author_displacement_bytecode.gdshaderinc\"\n" + \
	"#include \"res://shaders/terrain_author_material_bytecode.gdshaderinc\"\n"

var _blank_cache_state_applied: bool = false
var _blank_shader_active: bool = false
var _authored_shader_active: bool = false
var _displacement_runtime: Node
var _displacement_fingerprint: String = ""
var _material_runtime: Node
var _material_fingerprint: String = ""


func _ready() -> void:
	super._ready()
	_ensure_authoring_runtimes()
	_ensure_backend_shader(_blank_backend())
	_sync_authoring_displacement(true)
	_sync_authoring_material(true)
	_sync_authoring_shader_variant(true)


func _process(dt: float) -> void:
	var blank := _blank_backend()
	_ensure_backend_shader(blank)
	_set_cache_nodes_processing(not blank)
	_sync_authoring_displacement(false)
	_sync_authoring_material(false)
	_sync_authoring_shader_variant(false)
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


func _ensure_authoring_runtimes() -> void:
	_ensure_displacement_runtime()
	_ensure_material_runtime()


func _ensure_displacement_runtime() -> void:
	if _displacement_runtime != null and is_instance_valid(_displacement_runtime):
		return
	_displacement_runtime = DISPLACEMENT_RUNTIME_SCRIPT.new() as Node
	if _displacement_runtime == null:
		return
	_displacement_runtime.name = "TerrainDisplacementRuntime"
	add_child(_displacement_runtime)


func _ensure_material_runtime() -> void:
	if _material_runtime != null and is_instance_valid(_material_runtime):
		return
	_material_runtime = MATERIAL_RUNTIME_SCRIPT.new() as Node
	if _material_runtime == null:
		return
	_material_runtime.name = "TerrainMaterialRuntime"
	add_child(_material_runtime)


func _active_authoring_terrain() -> Resource:
	var editor: Node = get_tree().root.find_child("PlanetStudioLive", true, false)
	if editor == null:
		return null
	var session_value: Variant = editor.get("_session")
	if session_value == null or not session_value.has_method("active_terrain_profile"):
		return null
	return session_value.call("active_terrain_profile") as Resource


func _has_enabled_authoring_graph(terrain: Resource, property_name: StringName,
		domain: int) -> bool:
	if terrain == null:
		return false
	var slots_value: Variant = terrain.get(property_name)
	if not (slots_value is Array):
		return false
	for slot_value: Variant in slots_value as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.get(&"enabled")):
			continue
		if int(slot.get(&"domain")) != domain:
			continue
		if slot.get(&"graph") is Resource:
			return true
	return false


func _sync_authoring_displacement(force: bool) -> void:
	_ensure_displacement_runtime()
	if _displacement_runtime == null:
		return
	var terrain: Resource = _active_authoring_terrain()
	if terrain == null:
		return
	if not _has_enabled_authoring_graph(terrain, &"displacement_slots", 0):
		# Opening Planet Studio with a clean profile must be a GPU no-op. Do not
		# allocate/upload even the one-row fallback bytecode texture just because the
		# editor node became visible. If the last graph was deleted, explicitly turn
		# the old program off once and then stay dormant.
		if _displacement_fingerprint != AUTHORING_NONE_FINGERPRINT:
			_displacement_fingerprint = AUTHORING_NONE_FINGERPRINT
			_displacement_runtime.call("clear")
			if _material != null:
				_material.set_shader_parameter("u_author_disp_ready", 0.0)
		return
	var fingerprint: String = String(
		_displacement_runtime.call("profile_fingerprint", terrain))
	if force or fingerprint != _displacement_fingerprint:
		_displacement_fingerprint = fingerprint
		_displacement_runtime.call("compile_from_terrain", terrain)
	if _material != null and _displacement_runtime.has_method("bind_material"):
		_displacement_runtime.call("bind_material", _material)


func _sync_authoring_material(force: bool) -> void:
	_ensure_material_runtime()
	if _material_runtime == null:
		return
	var terrain: Resource = _active_authoring_terrain()
	if terrain == null:
		return
	if not _has_enabled_authoring_graph(terrain, &"material_slots", 1):
		# Same launch invariant as displacement: an empty staged profile must not
		# create an ImageTexture, replace sampler bindings, or otherwise perturb the
		# resident terrain material merely because Planet Studio was opened.
		if _material_fingerprint != AUTHORING_NONE_FINGERPRINT:
			_material_fingerprint = AUTHORING_NONE_FINGERPRINT
			_material_runtime.call("clear")
			if _material != null:
				_material.set_shader_parameter("u_author_mat_ready", 0.0)
		return
	var fingerprint: String = String(
		_material_runtime.call("profile_fingerprint", terrain))
	if force or fingerprint != _material_fingerprint:
		_material_fingerprint = fingerprint
		_material_runtime.call("compile_from_terrain", terrain)
	if _material != null and _material_runtime.has_method("bind_material"):
		_material_runtime.call("bind_material", _material)


func _ensure_backend_shader(blank: bool) -> void:
	if _material == null or blank == _blank_shader_active:
		return
	_install_backend_shader(blank, _authored_shader_active)


func _sync_authoring_shader_variant(force: bool) -> void:
	if _material == null:
		return
	var authored: bool = _runtime_program_active(_displacement_runtime) \
		or _runtime_program_active(_material_runtime)
	# Trusting `_authored_shader_active` alone is not enough: another node can
	# reassign this same `_material.shader` every frame (the retired runtime-shader
	# inspector force-restores the base shader whenever the material carries a
	# path-less runtime variant). Detect when the material is not actually carrying
	# the variant the flag claims and reinstall, so authored displacement is not
	# silently stripped for the rest of the session.
	var installed_variant: bool = _material.shader != null \
		and String(_material.shader.resource_name).begins_with("AsterraAuthoredTerrain")
	if not force and authored == _authored_shader_active and authored == installed_variant:
		return
	_install_backend_shader(_blank_backend(), authored)


func _runtime_program_active(runtime: Node) -> bool:
	if runtime == null or not is_instance_valid(runtime) or not runtime.has_method("stats"):
		return false
	var value: Variant = runtime.call("stats")
	return value is Dictionary and bool((value as Dictionary).get("active", false))


func _install_backend_shader(blank: bool, authored: bool) -> void:
	var path: String = BLANK_SHADER_PATH if blank else PROCEDURAL_SHADER_PATH
	if not ResourceLoader.exists(path, "Shader"):
		push_error("Terrain backend shader is missing: %s" % path)
		return
	var shader: Shader = load(path) as Shader
	if shader == null:
		push_error("Terrain backend shader failed to load: %s" % path)
		return
	if authored:
		var marker := "render_mode cull_front, diffuse_burley, specular_schlick_ggx"
		var marker_end := shader.code.find(";", shader.code.find(marker))
		if marker_end < 0:
			push_error("Terrain authored shader insertion marker is missing: %s" % path)
			return
		var source := shader.code.insert(marker_end + 1,
			"\n" + AUTHORED_SHADER_DEFINE)
		var include_marker := "void vertex()" if blank else \
			"#include \"res://shaders/terrain_clipmap_cache_sampling.gdshaderinc\""
		var include_at := source.find(include_marker)
		if include_at < 0:
			push_error("Terrain authored shader include marker is missing: %s" % path)
			return
		var includes := AUTHORED_BLANK_INCLUDES if blank else AUTHORED_PROCEDURAL_INCLUDES
		var candidate := Shader.new()
		candidate.resource_name = "AsterraAuthoredTerrainBlank" if blank \
			else "AsterraAuthoredTerrainProcedural"
		candidate.code = source.insert(include_at, includes + "\n")
		shader = candidate
	_material.shader = shader
	_blank_shader_active = blank
	_authored_shader_active = authored
	if blank:
		_apply_blank_material_state()
		_sync_authoring_displacement(true)
		_sync_authoring_material(true)
	else:
		# Shader replacement clears every binding. Restore the exact production
		# resources before allowing the inherited cache stack to run again.
		super._bind_gpu_resources(true)
		super._sync_material_control()
		_bind_gpu_context(true)
		_bind_surface_pbr(true)
		_sync_detail_seed()
		_sync_debug_uniforms()
		_sync_authoring_displacement(true)
		_sync_authoring_material(true)
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
	_sync_authoring_material(false)
	_sync_authoring_shader_variant(false)


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
	if _displacement_runtime != null and _displacement_runtime.has_method("bind_material") \
			and _displacement_fingerprint != AUTHORING_NONE_FINGERPRINT:
		_displacement_runtime.call("bind_material", _material)
	if _material_runtime != null and _material_runtime.has_method("bind_material") \
			and _material_fingerprint != AUTHORING_NONE_FINGERPRINT:
		_material_runtime.call("bind_material", _material)


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
	out["shader_displacement_enabled"] = true
	if _displacement_runtime != null and _displacement_runtime.has_method("stats"):
		out["author_displacement"] = _displacement_runtime.call("stats")
	if _material_runtime != null and _material_runtime.has_method("stats"):
		out["author_material"] = _material_runtime.call("stats")
	if _blank_backend():
		out["terrain_cache_active"] = false
		out["terrain_cache_ready"] = false
		out["coverage_ready"] = true
	return out


func _blank_backend() -> bool:
	return bool(Planet.get("blank_mode"))
