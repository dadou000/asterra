extends "res://scripts/terrain/gpu_terrain_scatter_compact.gd"
## Latest-0.0.5 GPU scatter binding.
##
## The current real runtime reports the indirect RenderingDevice compaction path as
## failed. Keep the proven vertex-GPU fallback authoritative for now: it still does
## all candidate placement/classification on the GPU, but avoids stale/invalid
## indirect buffers while the backend-specific failure is repaired separately.

const STABLE_FALLBACK_ONLY := true

var _static_scatter_bound := false
var _bound_edit_generation := -1
var _bound_edit_ready := false
var _bound_active_generation := -1
var _bound_active_window_generation := -1
var _bound_active_ready := false


func _ready() -> void:
	super._ready()
	if STABLE_FALLBACK_ONLY:
		_compact_method_supported = false
		_compact_init_failed = false
		_compact_init_ready = false
		_hide_compact_batches()


func _bind_gpu_resources(force: bool) -> void:
	if Planet.cfg == null:
		return

	var macro: Texture2DArray = Planet.global_height_texture if Planet.ready_state else null
	var macro_res: int = Planet.global_height_face_res if Planet.ready_state else 0
	if force or macro != _bound_macro or macro_res != _bound_macro_res:
		_bound_macro = macro
		_bound_macro_res = macro_res
		for material: ShaderMaterial in _materials:
			material.set_shader_parameter("u_scatter_macro_elevation", macro)
			material.set_shader_parameter("u_scatter_macro_face_res", float(macro_res))
			material.set_shader_parameter("u_scatter_macro_ready", 1.0 if macro != null else 0.0)

	var context: Node = get_node_or_null("/root/PlanetContext")
	if context == null or not bool(context.get("ready_state")):
		for material: ShaderMaterial in _materials:
			material.set_shader_parameter("u_ctx_ready", 0.0)
		return

	var generation: int = int(context.get("generation"))
	if force or generation != _bound_context_generation:
		_bound_context_generation = generation
		for material: ShaderMaterial in _materials:
			material.set_shader_parameter("u_ctx_soil", context.get("soil_texture"))
			material.set_shader_parameter("u_ctx_surface", context.get("surface_texture"))
			material.set_shader_parameter("u_ctx_geology", context.get("geology_texture"))
			material.set_shader_parameter("u_ctx_structure", context.get("structure_texture"))
			material.set_shader_parameter("u_ctx_climate", context.get("climate_texture"))
			material.set_shader_parameter("u_ctx_hydrology", context.get("hydrology_texture"))
			material.set_shader_parameter("u_ctx_rock", context.get("rock_texture"))
			material.set_shader_parameter("u_ctx_biome", context.get("biome_texture"))
			material.set_shader_parameter("u_ctx_face_res", float(context.get("face_res")))
			material.set_shader_parameter("u_ctx_ready", 1.0)

	if force or not _static_scatter_bound:
		var scatter_seed: int = Planet.cfg.stream_seed("gpu_scatter") & 0x00ffffff
		var detail_seed: int = Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff
		for material: ShaderMaterial in _materials:
			material.set_shader_parameter("u_scatter_seed", maxi(scatter_seed, 1))
			material.set_shader_parameter("u_scatter_detail_seed", maxi(detail_seed, 1))
			material.set_shader_parameter("u_scatter_geomorph_spacing", 0.75)
		_static_scatter_bound = true

	var edits: Node = get_node_or_null("/root/TerrainEditDeltaGPU")
	if edits != null and edits.has_method("sample_params"):
		var ep: Dictionary = edits.call("sample_params")
		var edit_generation: int = int(ep.get("generation", 0))
		var edit_ready: bool = bool(ep.get("ready", false))
		if force or edit_generation != _bound_edit_generation or edit_ready != _bound_edit_ready:
			for material: ShaderMaterial in _materials:
				material.set_shader_parameter("u_edit_delta", ep.get("texture"))
				material.set_shader_parameter("u_edit_ready", 1.0 if edit_ready else 0.0)
				material.set_shader_parameter("u_edit_center_dir", ep.get("center_dir", Vector3.RIGHT))
				material.set_shader_parameter("u_edit_center_right", ep.get("center_right", Vector3.BACK))
				material.set_shader_parameter("u_edit_center_up", ep.get("center_up", Vector3.UP))
				material.set_shader_parameter("u_edit_half_extent_m", float(ep.get("half_extent_m", 256.0)))
			_bound_edit_generation = edit_generation
			_bound_edit_ready = edit_ready

	var active: Node = get_node_or_null("/root/TerrainDeformationGPU")
	if active != null and active.has_method("sample_params"):
		var ap: Dictionary = active.call("sample_params")
		var active_generation: int = int(ap.get("generation", 0))
		var active_window_generation: int = int(ap.get("window_generation", 0))
		var active_ready: bool = bool(ap.get("ready", false))
		if force or active_generation != _bound_active_generation \
				or active_window_generation != _bound_active_window_generation \
				or active_ready != _bound_active_ready:
			for material: ShaderMaterial in _materials:
				material.set_shader_parameter("u_active_deform", ap.get("texture"))
				material.set_shader_parameter("u_active_deform_ready", 1.0 if active_ready else 0.0)
				material.set_shader_parameter("u_active_deform_center_dir", ap.get("center_dir", Vector3.RIGHT))
				material.set_shader_parameter("u_active_deform_center_right", ap.get("center_right", Vector3.BACK))
				material.set_shader_parameter("u_active_deform_center_up", ap.get("center_up", Vector3.UP))
				material.set_shader_parameter("u_active_deform_half_extent_m", float(ap.get("half_extent_m", 32.0)))
			_bound_active_generation = active_generation
			_bound_active_window_generation = active_window_generation
			_bound_active_ready = active_ready


func gpu_scatter_stats() -> Dictionary:
	return {
		"global_heightmap": true,
		"global_height_face_res": _bound_macro_res,
		"cpu_scatter_classification": false,
		"compute_supported": false if STABLE_FALLBACK_ONLY else _compact_method_supported,
		"compute_ready": false if STABLE_FALLBACK_ONLY else _compact_init_ready,
		"compute_failed": false if STABLE_FALLBACK_ONLY else _compact_init_failed,
		"stable_gpu_fallback": STABLE_FALLBACK_ONLY,
		"edit_delta_bound": get_node_or_null("/root/TerrainEditDeltaGPU") != null,
		"active_deform_bound": get_node_or_null("/root/TerrainDeformationGPU") != null,
	}
