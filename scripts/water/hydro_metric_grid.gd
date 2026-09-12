class_name HydroMetricGrid
extends RefCounted
## Metric contract between a cube-sphere HydroTileKey address and a fixed-resolution
## SWE tile.
##
## A quadtree address level is discrete. For a planet radius R, the nominal arc
## width of one cube face is PI/2*R, so a level-L tile has nominal width:
##
##   W(L) = (PI/2*R) / 2^L
##
## and a tile with N solver cells must use:
##
##   dx(L,N) = W(L) / N
##
## if terrain reconstruction, stable topology and SWE metres are to describe the
## same physical footprint. Production bootstrap should choose a level from its
## target dx, then initialize SparseHydroAtlasGPU with compatible_cell_size_m().

const FACE_ARC_RADIANS := PI * 0.5


static func face_arc_m(planet_radius_m: float) -> float:
	return FACE_ARC_RADIANS * maxf(planet_radius_m, 1.0)


static func tile_width_m(planet_radius_m: float, level: int) -> float:
	var l := clampi(level, 0, HydroTileKey.MAX_LEVEL)
	return face_arc_m(planet_radius_m) / pow(2.0, float(l))


static func compatible_cell_size_m(planet_radius_m: float,
		tile_resolution: int, level: int) -> float:
	return tile_width_m(planet_radius_m, level) / float(maxi(tile_resolution, 1))


static func level_for_target_cell_size(planet_radius_m: float,
		tile_resolution: int, target_cell_size_m: float) -> int:
	var n := maxi(tile_resolution, 1)
	var target_width := maxf(target_cell_size_m, 1.0e-6) * float(n)
	var ratio := face_arc_m(planet_radius_m) / target_width
	if ratio <= 1.0:
		return 0
	var level := int(round(log(ratio) / log(2.0)))
	return clampi(level, 0, HydroTileKey.MAX_LEVEL)


static func contract_for_target(planet_radius_m: float, tile_resolution: int,
		target_cell_size_m: float) -> Dictionary:
	var level := level_for_target_cell_size(
		planet_radius_m, tile_resolution, target_cell_size_m)
	var exact_dx := compatible_cell_size_m(planet_radius_m, tile_resolution, level)
	var target_dx := maxf(target_cell_size_m, 1.0e-6)
	return {
		"level": level,
		"tile_resolution": maxi(tile_resolution, 1),
		"target_cell_size_m": target_dx,
		"compatible_cell_size_m": exact_dx,
		"target_tile_width_m": target_dx * float(maxi(tile_resolution, 1)),
		"compatible_tile_width_m": tile_width_m(planet_radius_m, level),
		"cell_size_ratio": exact_dx / target_dx,
		"relative_error": absf(exact_dx - target_dx) / target_dx,
	}


static func atlas_contract(atlas: SparseHydroAtlasGPU,
		planet_radius_m: float) -> Dictionary:
	if atlas == null:
		return {}
	return contract_for_target(planet_radius_m, atlas.tile_resolution,
		atlas.cell_size_m)


static func atlas_is_metric_compatible(atlas: SparseHydroAtlasGPU,
		planet_radius_m: float, relative_tolerance: float = 1.0e-4) -> bool:
	var c := atlas_contract(atlas, planet_radius_m)
	return not c.is_empty() and float(c.get("relative_error", INF)) \
		<= maxf(relative_tolerance, 0.0)
