class_name WaterAuthoringProfile
extends Resource
## Shared authoring parameters for ocean, authored lakes and authored rivers.

@export var ocean_enabled: bool = true
@export var sea_level_m: float = 0.0
@export var wave_amplitude_scale: float = 1.0
@export var wave_frequency_scale: float = 1.0
@export var wind_response: float = 1.0
@export var foam_strength: float = 1.0
@export var absorption_scale: float = 1.0
@export var scattering_scale: float = 1.0
@export var authored_feature_ids: PackedStringArray = PackedStringArray()
@export var material_slots: Array[Dictionary] = []
