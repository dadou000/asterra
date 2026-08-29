class_name GenerationAuthoringProfile
extends Resource
## Serializable authoring copy of GenConfig. Planet Studio owns this copy so its
## persistence layer does not depend on the generator's global class registry.

@export var world_seed: int = 0x4153544552524100
@export var planet_radius: float = 1000000.0
@export var face_res: int = 192

@export var plate_count: int = 22
@export var ocean_fraction: float = 0.62
@export var continent_scale: float = 1.35
@export var max_uplift: float = 5400.0
@export var abyssal_depth: float = -4600.0
@export var shelf_depth: float = -160.0
@export var atmosphere_height: float = 60000.0

@export var landmark_density: float = 1.0
@export var volcanism_strength: float = 1.0
@export var max_volcanic_landmarks: int = 52

@export var strata_thickness: float = 180.0
@export var fault_density: float = 1.0
@export var ore_richness: float = 1.0

@export var erosion_iterations: int = 20
@export var stream_power_k: float = 3.2e-6
@export var stream_power_m: float = 0.45
@export var stream_power_n: float = 1.0
@export var erosion_dt: float = 2.0e4
@export var hillslope_relaxation: float = 0.075
@export var uplift_per_step: float = 32.0
@export var sediment_deposition: float = 0.35
@export var basin_infill_depth: float = 14.0
@export var basin_infill_excess: float = 0.30

@export var solar_constant: float = 1361.0
@export var olr_intercept: float = 206.0
@export var olr_slope: float = 2.09
@export var greenhouse_offset: float = 0.0
@export var heat_diffusion: float = 0.50
@export var albedo_ocean: float = 0.28
@export var albedo_land: float = 0.33
@export var albedo_ice: float = 0.60
@export var ice_onset_temp: float = -4.0
@export var ice_full_temp: float = -16.0

@export var anomaly_damping: float = 18.0
@export var seasonal_damping: float = 5.0
@export var ocean_mixed_layer: float = 55.0
@export var land_heat_capacity: float = 1.1e7
@export var marine_influence: float = 5.0
@export var lapse_rate: float = 0.0062

@export var base_precip: float = 950.0
@export var orographic_gain: float = 2.4
@export var axial_tilt_deg: float = 21.4

@export var detail_amplitude: float = 260.0
@export var detail_octaves: int = 6
@export var detail_base_frequency: float = 0.00035

@export var quadtree_max_depth: int = 16
@export var chunk_grid: int = 32
@export var lod_split_factor: float = 2.0
@export var lod_target_error_px: float = 2.25
@export var lod_collapse_ratio: float = 0.62
@export var collision_depth: int = 11
@export var collision_stream_depth: int = 15
@export var collision_stream_radius: float = 120.0
@export var collision_grid: int = 16
@export var edit_cell_size: float = 1.0
@export var use_gpu_bake: bool = true

func import_from_resource(source: Resource) -> void:
	if source == null:
		return
	var source_names: Dictionary = {}
	for source_property: Dictionary in source.get_property_list():
		source_names[StringName(source_property.get("name", ""))] = true
	for own_property: Dictionary in get_property_list():
		var property_name := StringName(own_property.get("name", ""))
		if property_name == &"script" or not source_names.has(property_name):
			continue
		set(property_name, source.get(property_name))

func copy_to_resource(target: Resource) -> void:
	if target == null:
		return
	var target_names: Dictionary = {}
	for target_property: Dictionary in target.get_property_list():
		target_names[StringName(target_property.get("name", ""))] = true
	for own_property: Dictionary in get_property_list():
		var property_name := StringName(own_property.get("name", ""))
		if property_name == &"script" or not target_names.has(property_name):
			continue
		target.set(property_name, get(property_name))
