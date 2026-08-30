class_name PlanetAuthoringProfile
extends Resource
## Per-terrestrial-body authoring bundle.
##
## `runtime_shader_paths` and `runtime_shader_overrides` are deliberately generic:
## they describe the exact file-backed Shader resources and uniform overrides used
## by Planet Studio's live renderer. They do not replace the higher-level terrain
## node graphs; they are an advanced, direct runtime inspection/editing layer.

const TERRAIN_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const WATER_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/water_authoring_profile.gd")
const ATMOSPHERE_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/atmosphere_profile.gd")

@export var terrain: Resource
@export var water: Resource
@export var atmosphere: Resource
@export var reference_sea_level_m: float = 0.0

# Stable target id -> res:// shader path. Only explicit user overrides are stored;
# absent targets continue to use the production renderer's built-in shader.
@export var runtime_shader_paths: Dictionary = {}
# Stable target id -> { uniform_name: Variant }. Overrides are re-applied after the
# production renderer updates its dynamic uniforms, so an authored value is the
# value actually seen by the GPU until it is cleared.
@export var runtime_shader_overrides: Dictionary = {}

func ensure_children() -> void:
	if terrain == null:
		terrain = TERRAIN_PROFILE_SCRIPT.new()
	if terrain.has_method("ensure_valid"):
		terrain.call("ensure_valid")
	else:
		terrain.call("ensure_generation_profile")
	if water == null:
		water = WATER_PROFILE_SCRIPT.new()
	if water.has_method("ensure_valid"):
		water.call("ensure_valid")
	if atmosphere == null:
		atmosphere = ATMOSPHERE_PROFILE_SCRIPT.new()
	if not (runtime_shader_paths is Dictionary):
		runtime_shader_paths = {}
	if not (runtime_shader_overrides is Dictionary):
		runtime_shader_overrides = {}

func runtime_shader_path(target_id: String) -> String:
	return String(runtime_shader_paths.get(target_id, ""))

func set_runtime_shader_path(target_id: String, path: String) -> void:
	if target_id.is_empty():
		return
	var next_path := path.strip_edges()
	if next_path.is_empty():
		runtime_shader_paths.erase(target_id)
	else:
		runtime_shader_paths[target_id] = next_path

func runtime_shader_uniform_overrides(target_id: String) -> Dictionary:
	var value: Variant = runtime_shader_overrides.get(target_id, {})
	return (value as Dictionary).duplicate(true) if value is Dictionary else {}

func set_runtime_shader_uniform(target_id: String, uniform_name: String, value: Variant) -> void:
	if target_id.is_empty() or uniform_name.is_empty():
		return
	var target: Dictionary = runtime_shader_uniform_overrides(target_id)
	target[uniform_name] = value
	runtime_shader_overrides[target_id] = target

func clear_runtime_shader_uniform(target_id: String, uniform_name: String) -> void:
	var target: Dictionary = runtime_shader_uniform_overrides(target_id)
	if not target.has(uniform_name):
		return
	target.erase(uniform_name)
	if target.is_empty():
		runtime_shader_overrides.erase(target_id)
	else:
		runtime_shader_overrides[target_id] = target

func clear_runtime_shader_target(target_id: String) -> void:
	runtime_shader_paths.erase(target_id)
	runtime_shader_overrides.erase(target_id)
