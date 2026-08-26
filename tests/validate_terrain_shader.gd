extends SceneTree
## Headless smoke test for the corrected latest-0.0.5 GPU terrain stack.

const SPATIAL_SHADERS := [
	"res://shaders/spherical_geometry_clipmap_procedural_uv.gdshader",
	"res://shaders/spherical_geometry_clipmap_global_gpu.gdshader",
	"res://shaders/spherical_geometry_clipmap_global_surface.gdshader",
	"res://shaders/spherical_geometry_clipmap_cached_surface.gdshader",
	"res://shaders/ocean_geometry_clipmap.gdshader",
	"res://shaders/terrain_scatter_grass.gdshader",
	"res://shaders/terrain_scatter_stone.gdshader",
	"res://shaders/terrain_scatter_compact_grass.gdshader",
	"res://shaders/terrain_scatter_compact_stone.gdshader",
]

const COMPUTE_SHADERS := [
	"res://shaders/terrain_scatter_compact.glsl",
	"res://shaders/terrain_height_query.glsl",
	"res://shaders/terrain_clipmap_cache.glsl",
	"res://shaders/terrain_occlusion.glsl",
]


func _init() -> void:
	for shader_path: String in SPATIAL_SHADERS:
		var resource: Resource = load(shader_path)
		if resource == null or not (resource is Shader):
			push_error("TERRAIN_SHADER_LOAD_FAILED: %s" % shader_path)
			quit(1)
			return
		var shader := resource as Shader
		var material := ShaderMaterial.new()
		material.shader = shader
		if not shader.get_rid().is_valid():
			push_error("TERRAIN_SHADER_RID_INVALID: %s" % shader_path)
			quit(1)
			return
		print("TERRAIN_SHADER_LOAD_OK: %s" % shader_path)

	for shader_path: String in COMPUTE_SHADERS:
		var resource: Resource = load(shader_path)
		if resource == null or not (resource is RDShaderFile):
			push_error("TERRAIN_COMPUTE_LOAD_FAILED: %s" % shader_path)
			quit(1)
			return
		var spirv: RDShaderSPIRV = (resource as RDShaderFile).get_spirv()
		if spirv == null:
			push_error("TERRAIN_COMPUTE_SPIRV_MISSING: %s" % shader_path)
			quit(1)
			return
		if not spirv.compile_error_compute.is_empty():
			push_error("TERRAIN_COMPUTE_COMPILE_FAILED: %s\n%s" % [shader_path, spirv.compile_error_compute])
			quit(1)
			return
		if spirv.bytecode_compute.is_empty():
			push_error("TERRAIN_COMPUTE_BYTECODE_EMPTY: %s" % shader_path)
			quit(1)
			return
		print("TERRAIN_COMPUTE_LOAD_OK: %s" % shader_path)

	print("TERRAIN_SHADER_STACK_OK: %d spatial + %d compute" % [
		SPATIAL_SHADERS.size(), COMPUTE_SHADERS.size()])
	quit(0)