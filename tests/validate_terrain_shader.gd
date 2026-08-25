extends SceneTree
## Headless smoke test for the active GPU terrain + scatter shader stack.
##
## Loading each top-level Shader forces Godot to preprocess its includes and send
## the resulting code through the renderer shader path. CI also scans the engine
## log for parser/compiler errors because an invalid Shader resource can still
## exist long enough for a script to receive a reference to it.

const SHADERS := [
	"res://shaders/spherical_geometry_clipmap_procedural_uv.gdshader",
	"res://shaders/terrain_scatter_grass.gdshader",
	"res://shaders/terrain_scatter_stone.gdshader",
]


func _init() -> void:
	for shader_path: String in SHADERS:
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

	print("TERRAIN_SHADER_STACK_OK: %d shaders" % SHADERS.size())
	quit(0)
