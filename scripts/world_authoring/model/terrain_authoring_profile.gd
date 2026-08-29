class_name TerrainAuthoringProfile
extends Resource
## Non-destructive authoring state layered on top of the pristine generator.

@export var generation_config: GenConfig
@export var biome_override_layer_ids: PackedStringArray = PackedStringArray()
@export var displacement_slots: Array[Dictionary] = []
@export var material_slots: Array[Dictionary] = []
@export var imported_texture_asset_ids: PackedStringArray = PackedStringArray()

func ensure_generation_config() -> GenConfig:
	if generation_config == null:
		generation_config = GenConfig.new()
	return generation_config
