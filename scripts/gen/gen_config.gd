class_name GenConfig
extends Resource
## Everything the planet generator needs. Two worlds with the same GenConfig are
## bit-identical; nothing in the pipeline may read state from outside this object.

## Bump when a generation pass changes, so stale bakes are not reused.
const PIPELINE_VERSION := 8

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
@export var polar_bias: float = 1.18               ## >1 cools faster with latitude than Earth
## Top of the atmosphere above sea level. Shared by the sky and by the aerial
## perspective on terrain and water, which have to agree or the horizon splits.
@export var atmosphere_height: float = 60000.0

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

## --- Climate ---
@export var equator_temp: float = 27.0             ## deg C annual mean at the equator, sea level
@export var pole_temp: float = -24.0
@export var lapse_rate: float = 0.0062             ## deg C per metre
@export var base_precip: float = 950.0             ## mm/yr
@export var orographic_gain: float = 2.4
@export var axial_tilt_deg: float = 21.4

## --- Runtime terrain detail ---
@export var detail_amplitude: float = 260.0
@export var detail_octaves: int = 6
@export var detail_base_frequency: float = 0.00035

## --- Streaming ---
@export var quadtree_max_depth: int = 16
@export var chunk_grid: int = 32                   ## vertices per chunk edge - 1
@export var lod_split_factor: float = 2.0
@export var collision_depth: int = 11              ## build collision at or beyond this depth

## --- Editing ---
@export var edit_cell_size: float = 1.0            ## metres between terrain-delta samples

@export var use_gpu_bake: bool = true              ## compute-shader fast path when available

func cache_key() -> String:
	var parts := [
		PIPELINE_VERSION, world_seed, planet_radius, face_res, plate_count, ocean_fraction,
		continent_scale, max_uplift, abyssal_depth, shelf_depth, polar_bias,
		strata_thickness, fault_density, ore_richness,
		erosion_iterations, stream_power_k, stream_power_m, stream_power_n,
		erosion_dt, hillslope_relaxation, uplift_per_step, sediment_deposition, basin_infill_depth, basin_infill_excess,
		equator_temp, pole_temp, lapse_rate, base_precip, orographic_gain,
	]
	return String(",").join(parts.map(func(v): return str(v))).sha256_text().substr(0, 16)

func stream_seed(label: String) -> int:
	return HashRNG.stream(world_seed, label)
