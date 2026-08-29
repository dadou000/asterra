class_name TerrainAuthoringProfile
extends Resource
## Non-destructive authoring state layered on top of the pristine generator.
##
## Procedural terrain remains reproducible from generation_profile. Sparse sculpt
## deltas are serialized separately so hand-authored terrain survives presets,
## Undo/Redo, Apply/Revert and body switching without baking a second heightmap.

const GENERATION_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/generation_authoring_profile.gd")
const BIOME_LAYER_SCRIPT := preload("res://scripts/world_authoring/model/biome_paint_layer.gd")
const SHADER_SLOT_SCRIPT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")

@export var generation_profile: Resource
@export var biome_override_layers: Array[Resource] = []
@export var displacement_slots: Array[Resource] = []
@export var material_slots: Array[Resource] = []
@export var imported_texture_asset_ids: PackedStringArray = PackedStringArray()

# Serialized form of the authoritative Deltas sparse cube-sphere lattice. The
# format mirrors Deltas.serialize(): one tile key list plus a packed float blob.
@export var sculpt_delta_version: int = 1
@export var sculpt_delta_keys: PackedInt64Array = PackedInt64Array()
@export var sculpt_delta_tiles: PackedByteArray = PackedByteArray()

func ensure_generation_profile() -> Resource:
	if generation_profile == null:
		generation_profile = GENERATION_PROFILE_SCRIPT.new()
	return generation_profile

func ensure_valid() -> void:
	ensure_generation_profile()
	sculpt_delta_version = maxi(1, sculpt_delta_version)
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

func set_sculpt_delta_serialized(data: Dictionary) -> void:
	sculpt_delta_version = maxi(1, int(data.get("version", 1)))
	var keys_value: Variant = data.get("keys", PackedInt64Array())
	var tiles_value: Variant = data.get("tiles", PackedByteArray())
	sculpt_delta_keys = (keys_value as PackedInt64Array).duplicate() if keys_value is PackedInt64Array else PackedInt64Array()
	sculpt_delta_tiles = (tiles_value as PackedByteArray).duplicate() if tiles_value is PackedByteArray else PackedByteArray()

func sculpt_delta_serialized() -> Dictionary:
	return {
		"version": sculpt_delta_version,
		"keys": sculpt_delta_keys.duplicate(),
		"tiles": sculpt_delta_tiles.duplicate(),
	}

func sculpt_delta_matches(data: Dictionary) -> bool:
	var keys_value: Variant = data.get("keys", PackedInt64Array())
	var tiles_value: Variant = data.get("tiles", PackedByteArray())
	if not (keys_value is PackedInt64Array) or not (tiles_value is PackedByteArray):
		return false
	return sculpt_delta_version == maxi(1, int(data.get("version", 1))) \
		and sculpt_delta_keys == (keys_value as PackedInt64Array) \
		and sculpt_delta_tiles == (tiles_value as PackedByteArray)

func has_sculpt_deltas() -> bool:
	return not sculpt_delta_keys.is_empty() and not sculpt_delta_tiles.is_empty()

func sculpt_edited_tile_count() -> int:
	return sculpt_delta_keys.size()

func clear_sculpt_deltas() -> void:
	sculpt_delta_version = 1
	sculpt_delta_keys = PackedInt64Array()
	sculpt_delta_tiles = PackedByteArray()

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
