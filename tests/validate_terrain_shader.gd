extends SceneTree
## Headless smoke test for the corrected latest-0.0.5 terrain shader stack.

const SPATIAL_SHADERS := [
	"res://shaders/spherical_geometry_clipmap_procedural_uv.gdshader",
	"res://shaders/spherical_geometry_clipmap_global_gpu.gdshader",
	"res://shaders/spherical_geometry_clipmap_global_surface.gdshader",
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
		var shader_rid := shader.get_rid()
		if not shader_rid.is_valid():
			push_error("TERRAIN_SHADER_RID_INVALID: %s" % shader_path)
			quit(1)
			return
		print("TERRAIN_SHADER_LOAD_OK: %s" % shader_path)

	print("TERRAIN_SHADER_STACK_OK: %d spatial" % SPATIAL_SHADERS.size())
	quit(0)
