extends "res://scripts/gen/planet_sampler_smooth2x.gd"
## GPU-first runtime Planet sampler.
##
## PROCEDURAL owns resident generated height/material maps. BLANK is deliberately
## different: there is no generated heightmap or material map to sample. Its base
## surface is the analytic body sphere and the renderer may add shader-authored
## displacement above that surface.

var blank_mode: bool = false


func adopt(p_fields: PlanetFields) -> void:
	blank_mode = false
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


func set_blank_mode(enabled: bool) -> void:
	if blank_mode == enabled:
		return
	blank_mode = enabled
	if blank_mode:
		# These are set to null rather than filled with zero images. A Blank body has
		# no generated height/material map by definition.
		global_height_texture = null
		global_height_face_res = 0
		global_height_cache_hit = false
		global_height_cache_path = ""
		global_height_compressed_bytes = 0
		global_height_raw_bytes = 0
		global_height_build_ms = 0
		orbit_elevation_texture = null
		orbit_texture_face_res = 0
		global_material_texture = null
		global_material_face_res = 0
		global_material_cache_hit = false
		global_material_cache_path = ""
		global_material_compressed_bytes = 0
		global_material_raw_bytes = 0
		global_material_build_ms = 0
		ready_state = true


## Coarse CPU fallback only. BLANK is exactly the analytic sphere: dormant Deltas
## are intentionally ignored because the blank backend permits shader displacement
## and custom biome paint, not a persistent authored heightfield.
func terrain_height(d: Vector3, _detail: TerrainDetail = null, snap: Dictionary = {}) -> float:
	if blank_mode:
		return 0.0
	var macro_h := macro_height(d)
	var h := macro_h + coast_profile_offset(d, macro_h)
	if snap.is_empty():
		h += Deltas.offset_at(d)
	else:
		h += Deltas.offset_at_snapshot(d, snap)
	return h


func pristine_height(d: Vector3, _detail: TerrainDetail = null) -> float:
	if blank_mode:
		return 0.0
	var macro_h := macro_height(d)
	return macro_h + coast_profile_offset(d, macro_h)


func radius_at(d: Vector3, detail: TerrainDetail = null, snap: Dictionary = {}) -> float:
	return cfg.planet_radius + terrain_height(d, detail, snap)


func surface_world(d: Vector3, detail: TerrainDetail = null) -> Vec3D:
	var r := radius_at(d, detail)
	return Vec3D.new(d.x * r, d.y * r, d.z * r)


func runtime_height_mode() -> String:
	return "analytic blank sphere / shader displacement" if blank_mode \
		else "GPU precise / CPU coarse fallback"
