class_name GenConfig
extends Resource
## Everything the planet generator needs. Two worlds with the same GenConfig are
## bit-identical; nothing in the pipeline may read state from outside this object.

## Bump when a generation pass changes, so stale bakes are not reused.
const PIPELINE_VERSION := 11

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
## Temperature is solved, not authored. These are the terms of the budget the
## zonal energy-balance model closes; see `gen/climate_ebm.gd`. Defaults are
## Earth-calibrated, so a fresh GenConfig describes an Earth-like climate and
## every departure from it is a deliberate one.
@export var solar_constant: float = 1361.0         ## W/m^2 at the planet's orbit (Sol at 1 AU)
## Outgoing longwave radiation, linearised as A + B*T (Budyko). A and B carry the
## greenhouse effect implicitly, which is what lets an energy balance work
## without a radiative-transfer column. A is calibrated here rather than quoted:
## fed Earth's insolation, tilt and land distribution, this pair reproduces
## Earth's observed profile (15 C global mean, 27 C equator, -15 C at 80 deg,
## 0.30 planetary albedo, 4% permanent ice). See tests/ClimateReport.tscn.
@export var olr_intercept: float = 206.0           ## A, W/m^2
@export var olr_slope: float = 2.09                ## B, W/m^2 per deg C
## Subtracted from A: positive warms the whole planet. This is the physically
## meaningful global-warmth knob -- the ice caps respond on their own.
@export var greenhouse_offset: float = 0.0         ## W/m^2
## Meridional heat transport, as diffusion in x = sin(latitude). Larger flattens
## the equator-to-pole gradient; drop it far enough and the caps run away. Fitted
## against Earth alongside A, and Earth-calibrated in an absolute sense -- the
## non-dimensionalisation absorbs the planet radius, so Asterra is given Earth's
## transport efficiency rather than one derived from its own smaller size.
@export var heat_diffusion: float = 0.50           ## D, W/m^2 per deg C
## Planetary albedos, cloud effects included. Ice is what closes the feedback
## loop that decides where the caps stop.
@export var albedo_ocean: float = 0.28
@export var albedo_land: float = 0.33
@export var albedo_ice: float = 0.60
## Ice is keyed to the ANNUAL MEAN, so the threshold sits well below freezing:
## a place averaging 0 C is boreal forest, not an ice sheet, because its summer
## melts what its winter laid down. The midpoint here is Budyko's empirical
## -10 C ice line, which is what makes that distinction without simulating a
## seasonal snowpack.
@export var ice_onset_temp: float = -4.0            ## deg C where permanent ice starts
@export var ice_full_temp: float = -16.0           ## deg C where the surface is wholly ice

## --- Climate: local departures ---
## Two different dampings, because a spatial anomaly and a seasonal cycle are
## erased by different things. A single cold cell is mixed away laterally by the
## atmosphere within days, so it is damped hard; the seasonal cycle is
## hemisphere-wide and has nowhere to mix to, so only radiation damps it. Using
## one number for both is what makes naive models grow runaway ice.
@export var anomaly_damping: float = 18.0          ## W/m^2 per deg C, spatial
@export var seasonal_damping: float = 5.0          ## W/m^2 per deg C, seasonal
## Ocean mixed-layer depth. Thermal inertia, and therefore how strongly the sea
## flattens the seasons of everything downwind of it.
@export var ocean_mixed_layer: float = 55.0        ## m
@export var land_heat_capacity: float = 1.1e7      ## J/m^2 per deg C, soil + air column
## How much of the heat the circulation delivers to a band lands on its maritime
## edge rather than its interior. This is what makes a west coast mild and the
## continent behind it severe.
@export var marine_influence: float = 5.0
@export var lapse_rate: float = 0.0062             ## deg C per metre

## --- Climate: moisture ---
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
@export var lod_target_error_px: float = 2.25      ## maximum projected geometric error
@export var lod_collapse_ratio: float = 0.62       ## split/collapse hysteresis
@export var collision_depth: int = 11              ## build collision at or beyond this depth
@export var collision_stream_depth: int = 15       ## independent local physics tiles
@export var collision_stream_radius: float = 120.0 ## metres around the observer
@export var collision_grid: int = 16               ## quads per physics tile edge

## --- Editing ---
@export var edit_cell_size: float = 1.0            ## metres between terrain-delta samples

@export var use_gpu_bake: bool = true              ## compute-shader fast path when available

func cache_key() -> String:
	var parts := [
		PIPELINE_VERSION, world_seed, planet_radius, face_res, plate_count, ocean_fraction,
		continent_scale, max_uplift, abyssal_depth, shelf_depth,
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
