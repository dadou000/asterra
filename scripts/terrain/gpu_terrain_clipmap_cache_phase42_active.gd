extends "res://scripts/terrain/gpu_terrain_clipmap_cache_phase42.gd"
## Thin activation layer so the proven cache base keeps its historical shader path
## untouched while Phase 42A validates the shared-control compute contract.

const PHASE42_SHADER_PATH := "res://shaders/terrain_clipmap_cache_phase42.glsl"


func _try_initialize() -> void:
	if _init_requested or ready_state or failed or not supported:
		return
	var resource: Resource = load(PHASE42_SHADER_PATH)
	if resource == null or not (resource is RDShaderFile):
		return
	var spirv: RDShaderSPIRV = (resource as RDShaderFile).get_spirv()
	if spirv == null or not spirv.compile_error_compute.is_empty() \
			or spirv.bytecode_compute.is_empty():
		failed = true
		push_error("Phase 42 GPU terrain clipmap cache shader is invalid.")
		return
	_init_requested = true
	RenderingServer.call_on_render_thread(_render_initialize_phase42.bind(
		spirv, _geomorph_control_bytes.duplicate()))
