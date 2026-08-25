extends SceneTree
## Headless smoke test for the active GPU terrain shader stack.
##
## Loading the top-level Shader forces Godot to preprocess its includes and send
## the resulting code through the renderer shader path. CI also scans the engine
## log for shader compilation/parser errors because an invalid Shader resource can
## still exist long enough for a script to receive a reference to it.

const TERRAIN_SHADER := "res://shaders/spherical_geometry_clipmap_procedural_uv.gdshader"


func _init() -> void:
	var resource: Resource = load(TERRAIN_SHADER)
	if resource == null or not (resource is Shader):
		push_error("TERRAIN_SHADER_LOAD_FAILED: %s" % TERRAIN_SHADER)
		quit(1)
		return

	var shader := resource as Shader
	var material := ShaderMaterial.new()
	material.shader = shader
	# Force creation/use of the shader RID before exiting the headless frame.
	var shader_rid := shader.get_rid()
	if not shader_rid.is_valid():
		push_error("TERRAIN_SHADER_RID_INVALID: %s" % TERRAIN_SHADER)
		quit(1)
		return

	RenderingServer.sync()
	print("TERRAIN_SHADER_LOAD_OK: %s" % TERRAIN_SHADER)
	quit(0)
