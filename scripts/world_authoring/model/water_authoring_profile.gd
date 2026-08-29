class_name WaterAuthoringProfile
extends Resource
## Shared authoring parameters for ocean, authored lakes and authored rivers.

const WATER_FEATURE_SCRIPT := preload("res://scripts/world_authoring/model/water_feature_definition.gd")
const SHADER_SLOT_SCRIPT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")

@export var ocean_enabled: bool = true
@export var sea_level_m: float = 0.0
@export var wave_amplitude_scale: float = 1.0
@export var wave_frequency_scale: float = 1.0
@export var wind_response: float = 1.0
@export var foam_strength: float = 1.0
@export var absorption_scale: float = 1.0
@export var scattering_scale: float = 1.0
@export var authored_features: Array[Resource] = []
@export var material_slots: Array[Resource] = []

func ensure_valid() -> void:
	wave_amplitude_scale = maxf(0.0, wave_amplitude_scale)
	wave_frequency_scale = maxf(0.001, wave_frequency_scale)
	wind_response = maxf(0.0, wind_response)
	foam_strength = maxf(0.0, foam_strength)
	absorption_scale = maxf(0.0, absorption_scale)
	scattering_scale = maxf(0.0, scattering_scale)
	for feature: Resource in authored_features:
		if feature != null and feature.has_method("ensure_valid"):
			feature.call("ensure_valid")
	for slot: Resource in material_slots:
		if slot != null and slot.has_method("ensure_valid"):
			slot.set(&"domain", SHADER_SLOT_SCRIPT.Domain.MATERIAL)
			slot.call("ensure_valid")

func create_feature(feature_type: int, display_name: String = "Water Feature") -> Resource:
	var feature: Resource = WATER_FEATURE_SCRIPT.new()
	feature.set(&"feature_type", feature_type)
	feature.set(&"display_name", display_name)
	feature.call("ensure_valid")
	authored_features.append(feature)
	return feature

func find_feature(feature_id: String) -> Resource:
	for feature: Resource in authored_features:
		if feature != null and String(feature.get(&"feature_id")) == feature_id:
			return feature
	return null

func remove_feature(feature_id: String) -> bool:
	for index: int in authored_features.size():
		var feature: Resource = authored_features[index]
		if feature != null and String(feature.get(&"feature_id")) == feature_id:
			authored_features.remove_at(index)
			return true
	return false

func create_material_slot(display_name: String = "Water Material") -> Resource:
	var slot: Resource = SHADER_SLOT_SCRIPT.new()
	slot.set(&"display_name", display_name)
	slot.set(&"domain", SHADER_SLOT_SCRIPT.Domain.MATERIAL)
	slot.call("ensure_valid")
	material_slots.append(slot)
	return slot
