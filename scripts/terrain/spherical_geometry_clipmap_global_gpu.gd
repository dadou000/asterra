extends "res://scripts/terrain/spherical_geometry_clipmap_global.gd"
## GPU synthesis extension for the current 0.0.5 resident global clipmap.
##
## This class deliberately inherits the latest global renderer instead of
## replacing its topology/LOD implementation. Therefore the following remain
## authoritative and unchanged:
##   - compact UV-addressed centre/ring sectors,
##   - screen-space promotion of the centre disc through L0..L14,
##   - per-level stationary lattice snapping,
##   - double-precision stable anchor reconstruction,
##   - resident Planet.global_height_texture macro terrain,
##   - resident Planet.global_material_texture fallback,
##   - dynamic ring visible-instance restoration after sector/debug culling.

const GPU_GEOMORPH_SHADER_PATH := "res://shaders/spherical_geometry_clipmap_global_gpu.gdshader"

var _gpu_ctx_generation: int = -1


func _ready() -> void:
	# Let the complete latest 0.0.5 clipmap initialize its topology, instance
	# windows, stable anchor state and resident global textures first.
	super._ready()
	_material.shader = load(GPU_GEOMORPH_SHADER_PATH)
	# Shader replacement clears its parameter bindings, so explicitly restore the
	# latest global height/material resources before layering context on top.
	super._bind_gpu_resources(true)
	super._sync_material_control()
	_bind_gpu_context(true)


func _process(dt: float) -> void:
	# All movement, promoted-centre LOD selection, ring visibility and stable
	# lattice logic remain in the latest parent implementation.
	super._process(dt)
	_bind_gpu_context(false)


func _bind_gpu_context(force: bool) -> void:
	if _material == null:
		return
	var context: Node = get_node_or_null("/root/PlanetContext")
	if context == null or not bool(context.get("ready_state")):
		_material.set_shader_parameter("u_ctx_ready", 0.0)
		return

	var generation: int = int(context.get("generation"))
	if force or generation != _gpu_ctx_generation:
		_gpu_ctx_generation = generation
		_material.set_shader_parameter("u_ctx_soil", context.get("soil_texture"))
		_material.set_shader_parameter("u_ctx_surface", context.get("surface_texture"))
		_material.set_shader_parameter("u_ctx_geology", context.get("geology_texture"))
		_material.set_shader_parameter("u_ctx_structure", context.get("structure_texture"))
		_material.set_shader_parameter("u_ctx_climate", context.get("climate_texture"))
		_material.set_shader_parameter("u_ctx_hydrology", context.get("hydrology_texture"))
		_material.set_shader_parameter("u_ctx_rock", context.get("rock_texture"))
		_material.set_shader_parameter("u_ctx_biome", context.get("biome_texture"))
		_material.set_shader_parameter("u_ctx_face_res", float(context.get("face_res")))
	_material.set_shader_parameter("u_ctx_ready", 1.0)


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	var context: Node = get_node_or_null("/root/PlanetContext")
	out["gpu_geomorph_latest_clipmap_base"] = true
	out["gpu_context_ready"] = context != null and bool(context.get("ready_state"))
	out["gpu_context_generation"] = _gpu_ctx_generation
	return out
