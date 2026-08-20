extends Node
## Autoload `Planet`: the runtime query surface for the generated world.
##
## Everything downstream -- terrain meshing, physics, the excavator, the HUD, and
## eventually Phase 2's assemblies sitting on the ground -- reads the planet
## through this one object. Terrain height at any direction is:
##
##     macro (baked, eroded)  +  sub-grid detail (procedural)  +  player deltas
##
## which is exactly the roadmap's "regenerate untouched terrain from seed, store
## only deltas".

signal world_ready(fields: PlanetFields)

var cfg: GenConfig
var fields: PlanetFields
var grid: PlanetGrid
var ready_state: bool = false

var _detail_main: TerrainDetail

## Graphics-only cube-face elevation texture. Each layer contains one macro face
## plus a one-texel neighbour gutter. Orbit shaders use this instead of chunk
## vertex interpolation, so shoreline position no longer depends on terrain LOD.
var orbit_elevation_texture: Texture2DArray
var orbit_texture_face_res: int = 0

func configure(p_cfg: GenConfig) -> void:
	cfg = p_cfg
	Frames.set_planet_radius(cfg.planet_radius)
	Frames.axial_tilt_deg = cfg.axial_tilt_deg

func adopt(p_fields: PlanetFields) -> void:
	fields = p_fields
	grid = p_fields.grid
	cfg = p_fields.cfg
	Frames.set_planet_radius(cfg.planet_radius)
	_detail_main = TerrainDetail.new(cfg)
	# Everything below is derived, never serialised, and must exist before the
	# first chunk is meshed -- the mesher calls into it from worker threads, so
	# nothing here may be built lazily.
	_build_warp_noise()
	_build_cell_colors()
	_build_water_fields()
	_build_orbit_textures()
	ready_state = true
	world_ready.emit(fields)

func make_detail() -> TerrainDetail:
	return TerrainDetail.new(cfg)

var _warp_noise: FastNoiseLite
## Per-cell surface albedo, sampled bilinearly by `surface_color`.
var _cell_color := PackedColorArray()
## Free-water surface elevation per cell, with no sentinel values in it -- every
## entry is a real height in metres, so it can be interpolated.
var _water_surface := PackedFloat32Array()
## 1 where a cell holds ocean or lake, 0 where it does not.
var _water_mask := PackedFloat32Array()

func _build_warp_noise() -> void:
	_warp_noise = FastNoiseLite.new()
	_warp_noise.seed = cfg.stream_seed("biome_edge")
	_warp_noise.noise_type = FastNoiseLite.TYPE_SIMPLEX
	_warp_noise.frequency = 1.0 / maxf(1.0, grid.cell_size[0] * 0.6)
	_warp_noise.fractal_octaves = 3

func _warp_lookup(d: Vector3) -> Vector3:
	var s := cfg.planet_radius
	var amp := grid.cell_size[0] * 0.55 / cfg.planet_radius
	var x := d.x * s
	var y := d.y * s
	var z := d.z * s
	return (d + Vector3(
		_warp_noise.get_noise_3d(x, y, z),
		_warp_noise.get_noise_3d(y + 913.0, z, x),
		_warp_noise.get_noise_3d(z, x - 471.0, y)) * amp).normalized()

## Build six small float textures directly from the authoritative macro field.
## One virtual cell is added around every face. Those gutter samples are obtained
## through the direction-space sampler, so linear GPU filtering sees the same
## neighbour the CPU sees when a lookup reaches a cube edge.
func _build_orbit_textures() -> void:
	orbit_elevation_texture = null
	orbit_texture_face_res = grid.res
	var res: int = grid.res
	var tex_res: int = res + 2
	var cell_step: float = 2.0 / float(res)
	var images: Array[Image] = []

	for face in 6:
		var img := Image.create(tex_res, tex_res, false, Image.FORMAT_RF)
		for y in tex_res:
			var j: int = y - 1
			var v: float = (float(j) + 0.5) * cell_step - 1.0
			for x in tex_res:
				var i: int = x - 1
				var h: float
				if i >= 0 and i < res and j >= 0 and j < res:
					h = fields.elev[grid.idx(face, i, j)]
				else:
					var u: float = (float(i) + 0.5) * cell_step - 1.0
					var d := CubeSphere.face_uv_to_dir(face, u, v)
					h = grid.sample_bilinear(fields.elev, d)
				img.set_pixel(x, y, Color(h, 0.0, 0.0, 1.0))
		images.append(img)

	var texture_array := Texture2DArray.new()
	var err := texture_array.create_from_images(images)
	if err != OK:
		push_error("Failed to build orbit elevation texture array (%d)" % err)
		return
	orbit_elevation_texture = texture_array

# ------------------------------------------------------------------ height ---
## Baked macro elevation (m relative to sea level), bilinear.
func macro_height(d: Vector3) -> float:
	return grid.sample_bilinear(fields.elev, d)

## Full terrain height in metres relative to sea level.
func terrain_height(d: Vector3, detail: TerrainDetail = null, snap: Dictionary = {}) -> float:
	var det := detail if detail != null else _detail_main
	var h := grid.sample_bilinear(fields.elev, d)
	var relief := grid.sample_bilinear(fields.relief, d)
	var flat := clampf(grid.sample_bilinear(fields.floodplain, d) * 0.8
		+ grid.sample_bilinear(fields.wetland, d) * 0.5, 0.0, 1.0)
	var hardness: float = 1.7 - grid.sample_bilinear(fields.erodibility, d) * 0.55
	if h > 0.0:
		h += det.height(d, relief, flat, hardness)
		# Macro rivers cut a shallow trench so the valley floor reads as a valley.
		var w := grid.sample_bilinear(fields.river_width, d)
		if w > 4.0:
			h -= clampf(w * 0.045, 0.0, 22.0)
	else:
		h += det.height(d, relief * 0.5, 0.55, 1.0)
	if snap.is_empty():
		h += Deltas.offset_at(d)
	else:
		h += Deltas.offset_at_snapshot(d, snap)
	return h

## Height with no player edits applied -- the "regenerate from seed" reference.
func pristine_height(d: Vector3, detail: TerrainDetail = null) -> float:
	var det := detail if detail != null else _detail_main
	var h := grid.sample_bilinear(fields.elev, d)
	var relief := grid.sample_bilinear(fields.relief, d)
	var flat := clampf(grid.sample_bilinear(fields.floodplain, d) * 0.8
		+ grid.sample_bilinear(fields.wetland, d) * 0.5, 0.0, 1.0)
	var hardness: float = 1.7 - grid.sample_bilinear(fields.erodibility, d) * 0.55
	if h > 0.0:
		h += det.height(d, relief, flat, hardness)
		var w := grid.sample_bilinear(fields.river_width, d)
		if w > 4.0:
			h -= clampf(w * 0.045, 0.0, 22.0)
	else:
		h += det.height(d, relief * 0.5, 0.55, 1.0)
	return h

func radius_at(d: Vector3, detail: TerrainDetail = null, snap: Dictionary = {}) -> float:
	return cfg.planet_radius + terrain_height(d, detail, snap)

func surface_world(d: Vector3, detail: TerrainDetail = null) -> Vec3D:
	var r := radius_at(d, detail)
	return Vec3D.new(d.x * r, d.y * r, d.z * r)

## Free water surface height at a direction (sea level, or a lake's level).
## Free-water surface elevation in metres. Interpolating `lake_level` directly is
## not an option: its "no lake here" value is -1e9, and blending that sentinel
## into a real level puts water vertices half a planet radius below the centre --
## which is what threw sheets of ocean geometry off into space around every lake.
func water_height(d: Vector3) -> float:
	return grid.sample_bilinear(_water_surface, d)

## 0..1 confidence that free water covers this direction, at macro-cell
## resolution. The waterline itself comes from the terrain crossing, not from
## this -- it only keeps a lake level from being painted across the next valley.
func water_coverage(d: Vector3) -> float:
	return grid.sample_bilinear(_water_mask, d)

func _build_water_fields() -> void:
	var n := grid.cell_count
	_water_surface = PackedFloat32Array()
	_water_surface.resize(n)
	_water_mask = PackedFloat32Array()
	_water_mask.resize(n)
	for c in n:
		var lake: float = fields.lake_level[c]
		var is_lake := lake > -1e8
		_water_surface[c] = lake if is_lake else 0.0
		_water_mask[c] = 1.0 if (is_lake or fields.elev[c] < 0.0) else 0.0
	# One ring of dilation. A lake's shoreline is a terrain crossing *inside* the
	# last wet cell, so the level has to still be the lake's own out at the cell
	# border; interpolating straight down to sea level instead sinks the water
	# surface into its own bank and eats the shallows.
	var dilated := _water_surface.duplicate()
	for c in n:
		if _water_mask[c] > 0.0:
			continue
		var best := 0.0
		var found := false
		for k in 8:
			var nb := grid.nbr[c * 8 + k]
			var lv: float = fields.lake_level[nb]
			if lv > -1e8 and (not found or lv > best):
				best = lv
				found = true
		if found:
			dilated[c] = best
	_water_surface = dilated

func _build_cell_colors() -> void:
	_cell_color = PackedColorArray()
	_cell_color.resize(grid.cell_count)
	for c in grid.cell_count:
		var col: Color = PlanetFields.BIOME_COLORS[fields.biome[c]]
		var rockiness := clampf(1.0 - fields.soil_depth[c] / 0.9, 0.0, 1.0)
		var rock_col := Color(0.42, 0.40, 0.38).lerp(Color(0.58, 0.55, 0.50),
			clampf(1.0 - PlanetFields.ROCK_ERODIBILITY[fields.rock[c]] / 1.9, 0.0, 1.0))
		_cell_color[c] = col.lerp(rock_col, rockiness * 0.65)

func has_water(d: Vector3) -> bool:
	var c := grid.dir_to_index(d)
	return fields.lake_level[c] > -1e8 or fields.elev[c] < 0.0

# ------------------------------------------------------------- inspection ---
## Everything known about a surface location. Used by the HUD, the excavator and
## (later) by settlement, agriculture and contract generation.
func sample_info(d: Vector3) -> Dictionary:
	var c := grid.dir_to_index(d)
	var latlon := CubeSphere.dir_to_latlon(d)
	return {
		"cell": c,
		"lat_deg": rad_to_deg(latlon.x),
		"lon_deg": rad_to_deg(latlon.y),
		"elevation": terrain_height(d),
		"macro_elevation": fields.elev[c],
		"relief": fields.relief[c],
		"rock": fields.rock[c],
		"rock_name": PlanetFields.ROCK_NAMES[fields.rock[c]],
		"strata_dip": fields.strata_dip[c],
		"fault": fields.fault[c],
		"basin": fields.basin[c],
		"ore_iron": fields.ore_iron[c],
		"ore_copper": fields.ore_copper[c],
		"coal": fields.coal[c],
		"petroleum": fields.petroleum[c],
		"gas_fraction": fields.gas_fraction[c],
		"quartz": fields.quartz[c],
		"aquifer": fields.aquifer[c],
		"sediment": fields.sediment[c],
		"discharge": fields.discharge[c],
		"river_width": fields.river_width[c],
		"stream_order": fields.stream_order[c],
		"watershed": fields.watershed[c],
		"lake": fields.lake_level[c] > -1e8,
		"floodplain": fields.floodplain[c],
		"wetland": fields.wetland[c],
		"temp_mean": fields.temp_mean[c],
		"temp_range": fields.temp_range[c],
		"precip": fields.precip[c],
		"humidity": fields.humidity[c],
		"wind": Vector2(fields.wind_u[c], fields.wind_v[c]),
		"storm_risk": fields.storm_risk[c],
		"soil_depth": fields.soil_depth[c],
		"soil_sand": fields.soil_sand[c],
		"soil_silt": fields.soil_silt[c],
		"soil_clay": fields.soil_clay[c],
		"soil_organic": fields.soil_organic[c],
		"soil_moisture": fields.soil_moisture[c],
		"biome": fields.biome[c],
		"biome_name": PlanetFields.BIOME_NAMES[fields.biome[c]],
		"vegetation": fields.vegetation[c],
		"suitability": fields.suitability[c],
		"corridor": fields.corridor[c],
	}

## What material sits `depth` metres below the surface at `d`.
## Soil horizons first, then weathered parent material, then the bedrock strata
## stack (which alternates families in sedimentary terrain).
func column_material(d: Vector3, depth: float) -> Dictionary:
	var c := grid.dir_to_index(d)
	var sd: float = grid.sample_bilinear(fields.soil_depth, d)
	var org: float = fields.soil_organic[c]
	var o_th := clampf(org * 0.55, 0.0, 0.45) * clampf(sd, 0.0, 1.0)
	var a_th := sd * 0.28
	var b_th := sd * 0.42
	var id := MaterialDB.ROCK
	if depth < o_th:
		id = MaterialDB.SOIL_O
	elif depth < o_th + a_th:
		id = MaterialDB.SOIL_A
	elif depth < o_th + a_th + b_th:
		id = MaterialDB.SOIL_B
	elif depth < sd:
		id = MaterialDB.SOIL_C
	var rock_family: int = fields.rock[c]
	if id == MaterialDB.ROCK:
		rock_family = strata_at(d, depth - sd)
	var props := MaterialDB.with_texture(id, fields.soil_sand[c], fields.soil_silt[c],
		fields.soil_clay[c], fields.soil_organic[c])
	props["id"] = id
	props["rock_family"] = rock_family
	props["name"] = MaterialDB.display_name(id, rock_family)
	if id == MaterialDB.ROCK:
		props["density"] = 2350.0 + PlanetFields.ROCK_ERODIBILITY[rock_family] * -420.0 + 500.0
		props["diggability"] = clampf(0.04 + PlanetFields.ROCK_ERODIBILITY[rock_family] * 0.10, 0.02, 0.35)
	props["ore_iron"] = fields.ore_iron[c]
	props["ore_copper"] = fields.ore_copper[c]
	props["quartz"] = fields.quartz[c]
	props["coal"] = fields.coal[c]
	return props

## Bedrock family at a given depth into rock, following the local bedding.
func strata_at(d: Vector3, depth_into_rock: float) -> int:
	var c := grid.dir_to_index(d)
	var surface: int = fields.rock[c]
	if not surface in [PlanetFields.Rock.SANDSTONE, PlanetFields.Rock.SHALE,
			PlanetFields.Rock.LIMESTONE, PlanetFields.Rock.DOLOMITE,
			PlanetFields.Rock.CONGLOMERATE]:
		return surface
	var layer := int(floor((depth_into_rock + fields.strata_phase[c]) / maxf(20.0, cfg.strata_thickness)))
	var seq := [PlanetFields.Rock.SANDSTONE, PlanetFields.Rock.SHALE,
		PlanetFields.Rock.LIMESTONE, PlanetFields.Rock.SHALE,
		PlanetFields.Rock.CONGLOMERATE, PlanetFields.Rock.DOLOMITE]
	var h := HashRNG.hash3(cfg.world_seed, fields.watershed[c], layer)
	return seq[absi(h) % seq.size()]

## Surface albedo used by the terrain shader and the orbital view.
##
## Two things keep the 8 km macro grid from reading as tiling: the lookup
## direction is jittered by a noise warp, so biome boundaries are organic rather
## than cell-shaped, and the palette itself is interpolated between cells instead
## of snapped to the nearest one. Nearest-cell lookup is what made the ground
## look tiled -- and rotated, because the cell axes turn with the cube face.
func surface_color(d: Vector3) -> Color:
	return grid.sample_color_bilinear(_cell_color, _warp_lookup(d))
