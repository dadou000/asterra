extends "res://scripts/gen/planet_sampler_smooth2x.gd"
## GPU-first runtime Planet sampler.
##
## CPU work stops at baked/resident coarse maps. `terrain_height()` is deliberately
## a coarse gameplay fallback and never calls TerrainDetail. Precise player ground
## contact is supplied asynchronously by TerrainHeightQuery from the same GPU
## macro + geomorph function used by the visible L0 terrain.


func adopt(p_fields: PlanetFields) -> void:
	fields = p_fields
	grid = p_fields.grid
	cfg = p_fields.cfg
	Frames.set_planet_radius(cfg.planet_radius)
	_detail_main = null
	_build_smoothed_macro_elevation()
	_build_coast_seaward_distance()
	_build_warp_noise()
	_build_cell_colors()
	_build_water_fields()
	_build_orbit_textures()
	ready_state = true
	world_ready.emit(fields)


## Coarse CPU fallback only. No procedural sub-grid synthesis occurs here.
func terrain_height(d: Vector3, _detail: TerrainDetail = null, snap: Dictionary = {}) -> float:
	var macro_h := macro_height(d)
	var h := macro_h + coast_profile_offset(d, macro_h)
	if snap.is_empty():
		h += Deltas.offset_at(d)
	else:
		h += Deltas.offset_at_snapshot(d, snap)
	return h


func pristine_height(d: Vector3, _detail: TerrainDetail = null) -> float:
	var macro_h := macro_height(d)
	return macro_h + coast_profile_offset(d, macro_h)


func radius_at(d: Vector3, detail: TerrainDetail = null, snap: Dictionary = {}) -> float:
	return cfg.planet_radius + terrain_height(d, detail, snap)


func surface_world(d: Vector3, detail: TerrainDetail = null) -> Vec3D:
	var r := radius_at(d, detail)
	return Vec3D.new(d.x * r, d.y * r, d.z * r)


func runtime_height_mode() -> String:
	return "GPU precise / CPU coarse fallback"
