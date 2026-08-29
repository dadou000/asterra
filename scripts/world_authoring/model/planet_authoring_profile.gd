class_name PlanetAuthoringProfile
extends Resource
## Per-terrestrial-body authoring bundle.

@export var terrain: TerrainAuthoringProfile
@export var water: WaterAuthoringProfile
@export var atmosphere: AtmosphereAuthoringProfile
@export var reference_sea_level_m: float = 0.0

func ensure_children() -> void:
	if terrain == null:
		terrain = TerrainAuthoringProfile.new()
	terrain.ensure_generation_config()
	if water == null:
		water = WaterAuthoringProfile.new()
	if atmosphere == null:
		atmosphere = AtmosphereAuthoringProfile.new()
