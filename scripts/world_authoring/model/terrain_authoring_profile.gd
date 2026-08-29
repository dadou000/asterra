class_name TerrainAuthoringProfile
extends Resource
## Non-destructive authoring state layered on top of the pristine generator.

const GENERATION_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/generation_authoring_profile.gd")
const BIOME_LAYER_SCRIPT := preload("res://scripts/world_authoring/model/biome_paint_layer.gd")
const SHADER_SLOT_SCRIPT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")

@export var generation_profile: Resource
@export var biome_override_layers: Array[Resource] = []
@export var displacement_slots: Array[Resource] = []
@export var material_slots: Array[Resource] = []
@export var imported_texture_asset_ids: PackedStringArray = PackedStringArray()

func ensure_generation_profile() -> Resource:
	if generation_profile == null:
		generation_profile = GENERATION_PROFILE_SCRIPT.new()
	return generation_profile

func ensure_valid() -> void:
	ensure_generation_profile()
	for layer: Resource in biome_override_layers:
		if layer != null and layer.has_method("ensure_valid"):
			layer.call("ensure_valid")
	for slot: Resource in displacement_slots:
		if slot != null and slot.has_method("ensure_valid"):
			slot.set(&"domain", SHADER_SLOT_SCRIPT.Domain.DISPLACEMENT)
			slot.call("ensure_valid")
	for slot: Resource in material_slots:
		if slot != null and slot.has_method("ensure_valid"):
			slot.set(&"domain", SHADER_SLOT_SCRIPT.Domain.MATERIAL)
			slot.call("ensure_valid")

func create_biome_layer(display_name: String = "Biome Paint") -> Resource:
	var layer: Resource = BIOME_LAYER_SCRIPT.new()
	layer.set(&"display_name", display_name)
	layer.call("ensure_valid")
	biome_override_layers.append(layer)
	return layer

func find_biome_layer(layer_id: String) -> Resource:
	for layer: Resource in biome_override_layers:
		if layer != null and String(layer.get(&"layer_id")) == layer_id:
			return layer
	return null

func remove_biome_layer(layer_id: String) -> bool:
	for index: int in biome_override_layers.size():
		var layer: Resource = biome_override_layers[index]
		if layer != null and String(layer.get(&"layer_id")) == layer_id:
			biome_override_layers.remove_at(index)
			return true
	return false

func create_shader_slot(domain: int, display_name: String = "Terrain Slot") -> Resource:
	var slot: Resource = SHADER_SLOT_SCRIPT.new()
	slot.set(&"display_name", display_name)
	slot.set(&"domain", domain)
	slot.call("ensure_valid")
	if domain == SHADER_SLOT_SCRIPT.Domain.MATERIAL:
		material_slots.append(slot)
	else:
		displacement_slots.append(slot)
	return slot

func find_shader_slot(slot_id: String) -> Resource:
	for slot: Resource in displacement_slots:
		if slot != null and String(slot.get(&"slot_id")) == slot_id:
			return slot
	for slot: Resource in material_slots:
		if slot != null and String(slot.get(&"slot_id")) == slot_id:
			return slot
	return null

func remove_shader_slot(slot_id: String) -> bool:
	for collection: Array in [displacement_slots, material_slots]:
		for index: int in collection.size():
			var slot: Resource = collection[index] as Resource
			if slot != null and String(slot.get(&"slot_id")) == slot_id:
				collection.remove_at(index)
				return true
	return false
