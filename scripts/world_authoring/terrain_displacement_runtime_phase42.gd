extends "res://scripts/world_authoring/terrain_displacement_runtime_phase41.gd"
## Phase 42A: publish the active transactional production geomorph snapshot to the
## renderer/cache bridge.
##
## This is read-only runtime state. Rejected graph candidates already restore
## `_production_controls` in Phase 33, so consumers can never observe settings that
## failed the displacement transaction. The shared GEOMORPH_CONTRACT is inherited
## from Phase 32, which also uses it for visible material binding.

# The authoring process polls the runtime every frame, but the production controls
# only change after a successful graph transaction.  A monotonically increasing
# revision lets material and cache consumers stay entirely idle between edits.
var _production_controls_revision: int = 0
var _production_controls_fingerprint: String = ""
var _last_bound_material_id: int = -1
var _last_bound_shader_id: int = -1
var _last_bound_compile_generation: int = -1
var _last_bound_controls_revision: int = -1


func clear() -> void:
	super.clear()
	_record_active_production_controls_revision()


func compile_from_terrain(terrain: Resource) -> Dictionary:
	var result: Dictionary = super.compile_from_terrain(terrain)
	# Phase 33 restores rejected candidates before this returns, so this records
	# only the controls that are actually visible and eligible to rebuild a cache.
	_record_active_production_controls_revision()
	return result


func bind_material(material: ShaderMaterial) -> void:
	if material == null:
		return
	if _material_binding_is_current(material):
		return
	super.bind_material(material)
	_remember_material_binding(material)


func bind_production_controls(material: ShaderMaterial) -> void:
	if material == null or _production_binding_is_current(material):
		return
	super.bind_production_controls(material)
	_remember_material_binding(material)


func active_production_controls() -> Dictionary:
	return GEOMORPH_CONTRACT.normalized_controls(_production_controls)


func active_production_controls_revision() -> int:
	return _production_controls_revision


func active_production_controls_fingerprint(fallback_seed: int) -> String:
	return GEOMORPH_CONTRACT.fingerprint(_production_controls, fallback_seed)


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["geomorph_gpu_contract_version"] = GEOMORPH_CONTRACT.CONTRACT_VERSION
	out["active_production_controls"] = active_production_controls()
	out["active_production_controls_revision"] = _production_controls_revision
	return out


func _record_active_production_controls_revision() -> void:
	# A constant fallback is sufficient here: this revision tracks authored control
	# values only. Planet seed/context changes already invalidate the terrain cache
	# through its world-ready binding generation.
	var fingerprint: String = GEOMORPH_CONTRACT.fingerprint(_production_controls, 1337)
	if fingerprint == _production_controls_fingerprint:
		return
	_production_controls_fingerprint = fingerprint
	_production_controls_revision += 1
	_last_bound_controls_revision = -1


func _material_binding_is_current(material: ShaderMaterial) -> bool:
	return material.get_instance_id() == _last_bound_material_id \
		and _shader_instance_id(material) == _last_bound_shader_id \
		and _compile_generation == _last_bound_compile_generation \
		and _production_controls_revision == _last_bound_controls_revision


func _production_binding_is_current(material: ShaderMaterial) -> bool:
	return material.get_instance_id() == _last_bound_material_id \
		and _shader_instance_id(material) == _last_bound_shader_id \
		and _production_controls_revision == _last_bound_controls_revision


func _remember_material_binding(material: ShaderMaterial) -> void:
	_last_bound_material_id = material.get_instance_id()
	_last_bound_shader_id = _shader_instance_id(material)
	_last_bound_compile_generation = _compile_generation
	_last_bound_controls_revision = _production_controls_revision


func _shader_instance_id(material: ShaderMaterial) -> int:
	var shader: Shader = material.shader
	return shader.get_instance_id() if shader != null else -1
