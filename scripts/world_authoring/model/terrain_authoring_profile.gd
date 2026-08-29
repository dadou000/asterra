class_name TerrainAuthoringProfile
extends Resource
## Non-destructive authoring state layered on top of the pristine generator.

const GENERATION_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/generation_authoring_profile.gd")

@export var generation_profile: Resource
@export var biome_override_layer_ids: PackedStringArray = PackedStringArray()
@export var displacement_slots: Array[Dictionary] = []
@export var material_slots: Array[Dictionary] = []
@export var imported_texture_asset_ids: PackedStringArray = PackedStringArray()

func ensure_generation_profile() -> Resource:
	if generation_profile == null:
		generation_profile = GENERATION_PROFILE_SCRIPT.new()
	return generation_profile
