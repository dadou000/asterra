class_name PlanetAuthoringProfile
extends Resource
## Per-terrestrial-body authoring bundle.

const TERRAIN_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const WATER_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/water_authoring_profile.gd")
const ATMOSPHERE_PROFILE_SCRIPT := preload("res://scripts/world_authoring/model/atmosphere_profile.gd")

@export var terrain: Resource
@export var water: Resource
@export var atmosphere: Resource
@export var reference_sea_level_m: float = 0.0

func ensure_children() -> void:
	if terrain == null:
		terrain = TERRAIN_PROFILE_SCRIPT.new()
	terrain.call("ensure_generation_profile")
	if water == null:
		water = WATER_PROFILE_SCRIPT.new()
	if atmosphere == null:
		atmosphere = ATMOSPHERE_PROFILE_SCRIPT.new()
