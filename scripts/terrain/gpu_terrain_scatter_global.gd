extends "res://scripts/terrain/gpu_terrain_scatter_compact.gd"
## Latest-0.0.5 GPU scatter binding.
##
## The current real runtime reports the indirect RenderingDevice compaction path as
## failed. Keep the proven vertex-GPU fallback authoritative for now: it still does
## all candidate placement/classification on the GPU, but avoids stale/invalid
## indirect buffers while the backend-specific failure is repaired separately.
##
## This layer also binds the same resident global height texture used by terrain.

const STABLE_FALLBACK_ONLY := true


func _ready() -> void:
	super._ready()
	if STABLE_FALLBACK_ONLY:
		# Parent schedules compute initialization deferred. Clearing support here
		# prevents that deferred attempt and makes the deterministic fallback the
		# intentional path rather than a consequence of a runtime failure.
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

	var scatter_seed: int = Planet.cfg.stream_seed("gpu_scatter") & 0x00ffffff
	var detail_seed: int = Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff
	for material: ShaderMaterial in _materials:
		material.set_shader_parameter("u_scatter_seed", maxi(scatter_seed, 1))
		material.set_shader_parameter("u_scatter_detail_seed", maxi(detail_seed, 1))
		material.set_shader_parameter("u_scatter_geomorph_spacing", 0.75)


func gpu_scatter_stats() -> Dictionary:
	return {
		"global_heightmap": true,
		"global_height_face_res": _bound_macro_res,
		"cpu_scatter_classification": false,
		"compute_supported": false if STABLE_FALLBACK_ONLY else _compact_method_supported,
		"compute_ready": false if STABLE_FALLBACK_ONLY else _compact_init_ready,
		"compute_failed": false if STABLE_FALLBACK_ONLY else _compact_init_failed,
		"stable_gpu_fallback": STABLE_FALLBACK_ONLY,
	}