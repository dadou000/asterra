class_name GenConfig
extends Resource
## Everything the planet generator needs. Two worlds with the same GenConfig are
## bit-identical; nothing in the pipeline may read state from outside this object.

## Bump when a generation pass changes, so stale bakes are not reused.
const PIPELINE_VERSION := 12

@export var world_seed: int = 0x4153544552524100   # "ASTERRA"
@export var planet_radius: float = 1000000.0        ## metres
@export var face_res: int = 192                    ## macro grid cells per cube face edge

## --- Macro geography ---
@export var plate_count: int = 22
@export var ocean_fraction: float = 0.62           ## target fraction of surface below sea level
@export var continent_scale: float = 1.35          ## larger = fewer, bigger landmasses
@export var max_uplift: float = 5400.0             ## metres of orogenic potential
@export var abyssal_depth: float = -4600.0
@export var shelf_depth: float = -160.0
## Top of the atmosphere above sea level. Shared by the sky and by the aerial
## perspective on terrain and water, which have to agree or the horizon splits.
@export var atmosphere_height: float = 60000.0

## --- Exceptional geologic landmarks ---
## These scale deterministic placement and relief; they do not directly choose
## how many cells are water or which biome occupies them.
@export var landmark_density: float = 1.0
@export var volcanism_strength: float = 1.0
@export var max_volcanic_landmarks: int = 52

## --- Geology ---
@export var strata_thickness: float = 180.0        ## metres per sedimentary layer
@export var fault_density: float = 1.0
@export var ore_richness: float = 1.0

## --- Erosion ---
@export var erosion_iterations: int = 20
@export var stream_power_k: float = 3.2e-6         ## incision coefficient
@export var stream_power_m: float = 0.45           ## drainage-area exponent
@export var stream_power_n: float = 1.0            ## slope exponent
@export var erosion_dt: float = 2.0e4              ## years per iteration
@export var hillslope_relaxation: float = 0.075    ## per-iteration hillslope smoothing, 0..0.25
@export var uplift_per_step: float = 32.0          ## metres of tectonic uplift per erosion iteration
@export var sediment_deposition: float = 0.35
@export var basin_infill_depth: float = 14.0      ## depressions shallower than this silt up completely, per iteration
@export var basin_infill_excess: float = 0.30     ## fraction of the remainder that also silts up, per iteration

## --- Climate: energy balance ---
@export var solar_constant: float = 1361.0         ## W/m^2 at the planet's orbit (Sol at 1 AU)
@export var olr_intercept: float = 206.0           ## A, W/m^2
@export var olr_slope: float = 2.09                ## B, W/m^2 per deg C
@export var greenhouse_offset: float = 0.0         ## W/m^2
@export var heat_diffusion: float = 0.50           ## D, W/m^2 per deg C
@export var albedo_ocean: float = 0.28
@export var albedo_land: float = 0.33
@export var albedo_ice: float = 0.60
@export var ice_onset_temp: float = -4.0
@export var ice_full_temp: float = -16.0

## --- Climate: local departures ---
@export var anomaly_damping: float = 18.0
@export var seasonal_damping: float = 5.0
@export var ocean_mixed_layer: float = 55.0
@export var land_heat_capacity: float = 1.1e7
@export var marine_influence: float = 5.0
@export var lapse_rate: float = 0.0062

## --- Climate: moisture ---
@export var base_precip: float = 950.0
@export var orographic_gain: float = 2.4
@export var axial_tilt_deg: float = 21.4

## --- Runtime terrain detail ---
@export var detail_amplitude: float = 260.0
@export var detail_octaves: int = 6
@export var detail_base_frequency: float = 0.00035

## --- Streaming ---
@export var quadtree_max_depth: int = 16
@export var chunk_grid: int = 32
@export var lod_split_factor: float = 2.0
@export var lod_target_error_px: float = 2.25
@export var lod_collapse_ratio: float = 0.62
@export var collision_depth: int = 11
@export var collision_stream_depth: int = 15
@export var collision_stream_radius: float = 120.0
@export var collision_grid: int = 16

## --- Editing ---
@export var edit_cell_size: float = 1.0

@export var use_gpu_bake: bool = true

func cache_key() -> String:
	var parts := [
		PIPELINE_VERSION, world_seed, planet_radius, face_res, plate_count, ocean_fraction,
		continent_scale, max_uplift, abyssal_depth, shelf_depth,
		landmark_density, volcanism_strength, max_volcanic_landmarks,
		strata_thickness, fault_density, ore_richness,
		erosion_iterations, stream_power_k, stream_power_m, stream_power_n,
		erosion_dt, hillslope_relaxation, uplift_per_step, sediment_deposition, basin_infill_depth, basin_infill_excess,
		solar_constant, olr_intercept, olr_slope, greenhouse_offset, heat_diffusion,
		albedo_ocean, albedo_land, albedo_ice, ice_onset_temp, ice_full_temp,
		anomaly_damping, seasonal_damping, ocean_mixed_layer, land_heat_capacity, marine_influence,
		lapse_rate, base_precip, orographic_gain, axial_tilt_deg,
	]
	return String(",").join(parts.map(func(v): return str(v))).sha256_text().substr(0, 16)

func stream_seed(label: String) -> int:
	return HashRNG.stream(world_seed, label)
