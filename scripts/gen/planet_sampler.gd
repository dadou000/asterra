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
signal coast_profile_changed

const MACRO_SMOOTH_PASSES := 2
const MACRO_SMOOTH_STRENGTH := 0.50
const COAST_PROFILE_PATH := "user://coastline_profile.cfg"

var cfg: GenConfig
var fields: PlanetFields
var grid: PlanetGrid
var ready_state: bool = false

var _detail_main: TerrainDetail

## Runtime terrain macro elevation. The bake remains untouched for geology,
## hydrology, climate, etc.; this derived field removes the visible 8 km cell
## lattice before the value is upsampled into terrain chunks and coastline caches.
var _macro_elev := PackedFloat32Array()
## Relief recomputed from the exact clamped/smoothed render elevation. Runtime
## detail must not be driven by spikes that no longer exist in its macro surface.
var _macro_relief := PackedFloat32Array()
## Geodesic metres from the macro shoreline, populated only on the ocean side.
## Land is exactly zero, so a profile can never leak inland.
var _coast_seaward_distance := PackedFloat32Array()
## Vector2(distance_metres, height_offset_metres), sorted by distance. The first
## point is always locked to (0, 0), preserving an unbroken land/sea boundary.
var _coast_profile_points := PackedVector2Array([
	Vector2(0.0, 0.0),
	Vector2(10000.0, 0.0),
	Vector2(50000.0, 0.0),
	Vector2(250000.0, 0.0),
	Vector2(1000000.0, 0.0),
])
var coast_profile_active := false

## Graphics-only cube-face elevation texture. Each layer contains one macro face
## plus a one-texel neighbour gutter. Orbit shaders use this instead of chunk
## vertex interpolation, so shoreline position no longer depends on terrain LOD.
var orbit_elevation_texture: Texture2DArray
var orbit_texture_face_res: int = 0

func configure(p_cfg: GenConfig) -> void:
	cfg = p_cfg
	Frames.set_planet_radius(cfg.planet_radius)
	Frames.axial_tilt_deg = cfg.axial_tilt_deg
	_load_coast_profile()

func adopt(p_fields: PlanetFields) -> void:
	fields = p_fields
	grid = p_fields.grid
	cfg = p_fields.cfg
	Frames.set_planet_radius(cfg.planet_radius)
	_detail_main = TerrainDetail.new(cfg)
	# Everything below is derived, never serialised, and must exist before the
	# first chunk is meshed -- the mesher calls into it from worker threads, so
	# nothing here may be built lazily.
	_build_smoothed_macro_elevation()
	_build_coast_seaward_distance()
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
## Per-cell packed biome material weights. Precomposing these once turns six
## spherical field lookups per terrain sample into one cache-friendly lookup.
var _cell_surface := PackedColorArray()
## Continuous geomorphology families: R mountain/crag, G arid/aeolian,
## B glacial/periglacial, A wet/depositional. Unlike the render material weights,
## these select landform processes and are consumed by TerrainDetail.
var _cell_geomorph := PackedColorArray()
## Free-water surface elevation per cell, with no sentinel values in it -- every
## entry is a real height in metres, so it can be interpolated.
var _water_surface := PackedFloat32Array()
## 1 where a cell holds ocean or lake, 0 where it does not.
var _water_mask := PackedFloat32Array()

## Mild separable-looking 3x3 Gaussian relaxation on the spherical neighbour
## graph. Two half-strength passes are enough to remove the baked cell corners and
## slope discontinuities without turning mountains into broad domes. Because the
## neighbour table crosses cube faces geometrically, this filter has no cube seam.
func _build_smoothed_macro_elevation() -> void:
	_macro_elev = fields.elev.duplicate()
	# The streamer and horizon culler are designed for a roughly Earth-like
	# elevation envelope. Rare erosion/cache spikes tens of kilometres tall are
	# numerical outliers, not plausible mountains; contain them in this derived
	# render field without mutating canonical geology, climate or save data.
	var runtime_floor := cfg.abyssal_depth * 1.42
	var runtime_ceiling := cfg.max_uplift + 1400.0
	for c in grid.cell_count:
		_macro_elev[c] = clampf(_macro_elev[c], runtime_floor, runtime_ceiling)
	for _pass in MACRO_SMOOTH_PASSES:
		var src := _macro_elev
		var dst := PackedFloat32Array()
		dst.resize(grid.cell_count)
		for c in grid.cell_count:
			var base := c * 8
			# Gaussian-like 3x3 kernel on E/NE/N/NW/W/SW/S/SE:
			#   1 2 1
			#   2 4 2   / 16
			#   1 2 1
			var blurred := src[c] * 4.0
			blurred += src[grid.nbr[base + 0]] * 2.0
			blurred += src[grid.nbr[base + 2]] * 2.0
			blurred += src[grid.nbr[base + 4]] * 2.0
			blurred += src[grid.nbr[base + 6]] * 2.0
			blurred += src[grid.nbr[base + 1]]
			blurred += src[grid.nbr[base + 3]]
			blurred += src[grid.nbr[base + 5]]
			blurred += src[grid.nbr[base + 7]]
			blurred *= 1.0 / 16.0
			dst[c] = lerpf(src[c], blurred, MACRO_SMOOTH_STRENGTH)
		_macro_elev = dst
	_macro_relief = PackedFloat32Array()
	_macro_relief.resize(grid.cell_count)
	for c in grid.cell_count:
		var h: float = _macro_elev[c]
		var relief := 0.0
		for k in 8:
			relief = maxf(relief, absf(h - _macro_elev[grid.nbr[c * 8 + k]]))
		_macro_relief[c] = relief

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

## Build six small float textures directly from the authoritative runtime macro
## field. One virtual cell is added around every face. Those gutter samples are
## obtained through the direction-space sampler, so filtering sees the same
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
					h = _macro_elev[grid.idx(face, i, j)]
				else:
					var u: float = (float(i) + 0.5) * cell_step - 1.0
					var d := CubeSphere.face_uv_to_dir(face, u, v)
					h = grid.sample_bilinear(_macro_elev, d)
				img.set_pixel(x, y, Color(h, 0.0, 0.0, 1.0))
		images.append(img)

	var texture_array := Texture2DArray.new()
	var err := texture_array.create_from_images(images)
	if err != OK:
		push_error("Failed to build orbit elevation texture array (%d)" % err)
		return
	orbit_elevation_texture = texture_array

# ------------------------------------------------------------------ height ---
## Baked macro elevation after the runtime smoothing pass, in metres relative to
## sea level. The same field is used by chunks, coastline clipmaps and orbit.
func macro_height(d: Vector3) -> float:
	return grid.sample_bilinear(_macro_elev, d)

func runtime_relief(d: Vector3) -> float:
	return grid.sample_bilinear(_macro_relief, d)

## RG is the baked downslope direction in the canonical east/north tangent
## frame, B is log-scaled discharge, and A is depositional influence. Runtime
## synthesis consumes this so its sub-grid tributaries inherit real watersheds.
func drainage_composition(d: Vector3) -> Color:
	var c := grid.dir_to_index(d)
	var flow_index := int(fields.flow_dir[c])
	var flow := Vector3.ZERO
	if flow_index >= 0 and flow_index < 8:
		var target := grid.cell_dir(grid.nbr[c * 8 + flow_index])
		flow = target - grid.cell_dir(c)
		flow -= d * flow.dot(d)
		if flow.length_squared() > 1e-12:
			flow = flow.normalized()
	var basis := CubeSphere.tangent_basis(d)
	var q := clampf(log(1.0 + maxf(float(fields.discharge[c]), 0.0)) / log(10001.0), 0.0, 1.0)
	var deposit := clampf(float(fields.floodplain[c]) * 0.75
		+ float(fields.wetland[c]) * 0.45, 0.0, 1.0)
	return Color(flow.dot(basis[0]), flow.dot(basis[1]), q, deposit)

## Build a planetwide graph distance from every ocean cell to the nearest
## land/ocean crossing. Coastal ocean cells are seeded at the interpolated zero
## crossing along their shortest neighbour edge, then a multi-source Dijkstra
## walk carries that distance across shelves and ocean basins.
func _build_coast_seaward_distance() -> void:
	var n: int = grid.cell_count
	_coast_seaward_distance = PackedFloat32Array()
	_coast_seaward_distance.resize(n)
	_coast_seaward_distance.fill(INF)
	var heap_cells: Array[int] = []
	var heap_dist: Array[float] = []

	for c in n:
		var h: float = _macro_elev[c]
		if h >= 0.0:
			_coast_seaward_distance[c] = 0.0
			continue
		var seed := INF
		for k in 8:
			var nb: int = grid.nbr[c * 8 + k]
			var nh: float = _macro_elev[nb]
			if nh < 0.0:
				continue
			var edge: float = 0.5 * (grid.cell_size[c] + grid.cell_size[nb])
			if (k & 1) != 0:
				edge *= 1.41421356237
			var crossing: float = clampf(-h / maxf(nh - h, 1e-6), 0.0, 1.0)
			seed = minf(seed, edge * crossing)
		if seed < INF:
			_coast_seaward_distance[c] = seed
			_coast_heap_push(heap_cells, heap_dist, c, seed)

	while not heap_cells.is_empty():
		var item := _coast_heap_pop(heap_cells, heap_dist)
		var c: int = item.x
		var distance: float = item.y
		if distance > _coast_seaward_distance[c] + 0.01:
			continue
		for k in 8:
			var nb: int = grid.nbr[c * 8 + k]
			if _macro_elev[nb] >= 0.0:
				continue
			var edge: float = 0.5 * (grid.cell_size[c] + grid.cell_size[nb])
			if (k & 1) != 0:
				edge *= 1.41421356237
			var candidate := distance + edge
			if candidate + 0.01 < _coast_seaward_distance[nb]:
				_coast_seaward_distance[nb] = candidate
				_coast_heap_push(heap_cells, heap_dist, nb, candidate)

	# A water body with no land on the generated planet is pathological but must
	# still produce finite samples. Treat it as being beyond the final profile key.
	var fallback: float = cfg.planet_radius * PI
	for c in n:
		if not is_finite(_coast_seaward_distance[c]):
			_coast_seaward_distance[c] = fallback

func _coast_heap_push(cells: Array[int], distances: Array[float], cell: int,
		distance: float) -> void:
	var i := cells.size()
	cells.append(cell)
	distances.append(distance)
	while i > 0:
		var parent := (i - 1) >> 1
		if distances[parent] <= distance:
			break
		cells[i] = cells[parent]
		distances[i] = distances[parent]
		i = parent
	cells[i] = cell
	distances[i] = distance

func _coast_heap_pop(cells: Array[int], distances: Array[float]) -> Vector2:
	var out := Vector2(cells[0], distances[0])
	var last_cell: int = cells.pop_back()
	var last_distance: float = distances.pop_back()
	if cells.is_empty():
		return out
	var i := 0
	while true:
		var left := i * 2 + 1
		if left >= cells.size():
			break
		var right := left + 1
		var child := right if right < cells.size() and distances[right] < distances[left] else left
		if distances[child] >= last_distance:
			break
		cells[i] = cells[child]
		distances[i] = distances[child]
		i = child
	cells[i] = last_cell
	distances[i] = last_distance
	return out

func coast_seaward_distance(d: Vector3) -> float:
	if not ready_state or _coast_seaward_distance.is_empty() or macro_height(d) >= 0.0:
		return 0.0
	return maxf(0.0, grid.sample_bilinear(_coast_seaward_distance, d))

func coast_profile_points() -> PackedVector2Array:
	return _coast_profile_points.duplicate()

func set_coast_profile_points(points: PackedVector2Array, persist := true) -> void:
	var ordered: Array[Vector2] = []
	for point in points:
		ordered.append(Vector2(maxf(point.x, 0.0), point.y))
	ordered.sort_custom(func(a: Vector2, b: Vector2) -> bool: return a.x < b.x)
	if ordered.size() < 2:
		ordered = [Vector2.ZERO, Vector2(1000000.0, 0.0)]
	ordered[0] = Vector2.ZERO
	_coast_profile_points = PackedVector2Array(ordered)
	coast_profile_active = false
	for point in _coast_profile_points:
		if absf(point.y) > 1e-6:
			coast_profile_active = true
			break
	if persist:
		_save_coast_profile()
	coast_profile_changed.emit()

func coast_profile_offset(d: Vector3, known_macro_h := INF) -> float:
	if not coast_profile_active:
		return 0.0
	var macro_h: float = macro_height(d) if not is_finite(known_macro_h) else known_macro_h
	if macro_h >= 0.0 or _coast_profile_points.size() < 2:
		return 0.0
	var distance := coast_seaward_distance(d)
	for i in _coast_profile_points.size() - 1:
		var a: Vector2 = _coast_profile_points[i]
		var b: Vector2 = _coast_profile_points[i + 1]
		if distance <= b.x:
			return lerpf(a.y, b.y, inverse_lerp(a.x, b.x, distance))
	return _coast_profile_points[-1].y

func _load_coast_profile() -> void:
	var config := ConfigFile.new()
	if config.load(COAST_PROFILE_PATH) != OK:
		return
	var raw: Variant = config.get_value("profile", "points", [])
	var points := PackedVector2Array()
	if raw is Array:
		for value in raw:
			if value is Vector2:
				points.append(value)
	if points.size() >= 2:
		set_coast_profile_points(points, false)

func _save_coast_profile() -> void:
	var config := ConfigFile.new()
	var points: Array[Vector2] = []
	for point in _coast_profile_points:
		points.append(point)
	config.set_value("profile", "points", points)
	var err := config.save(COAST_PROFILE_PATH)
	if err != OK:
		push_warning("Could not save coastline profile (%d)" % err)

## Detail parameters stay identical through the coast and continental shelf.
## Only genuinely deep ocean gradually approaches the old subdued seabed profile.
## This removes the hard terrain-amplitude discontinuity that used to occur at
## exactly 0 m macro elevation.
func _detail_profile(macro_h: float, relief: float, flat: float, hardness: float) -> Vector3:
	var ocean_depth := maxf(-macro_h, 0.0)
	var deep_ocean := smoothstep(250.0, 1500.0, ocean_depth)
	var used_relief := lerpf(relief, relief * 0.5, deep_ocean)
	var used_flat := lerpf(flat, maxf(flat, 0.55), deep_ocean)
	var used_hardness := lerpf(hardness, 1.0, deep_ocean)
	return Vector3(used_relief, used_flat, used_hardness)

## Full terrain height in metres relative to sea level.
func terrain_height(d: Vector3, detail: TerrainDetail = null, snap: Dictionary = {}) -> float:
	var det := detail if detail != null else _detail_main
	var macro_h := macro_height(d)
	var relief := runtime_relief(d)
	var flat := clampf(grid.sample_bilinear(fields.floodplain, d) * 0.8
		+ grid.sample_bilinear(fields.wetland, d) * 0.5, 0.0, 1.0)
	var hardness: float = 1.7 - grid.sample_bilinear(fields.erodibility, d) * 0.55
	var profile := _detail_profile(macro_h, relief, flat, hardness)
	var lookup := surface_lookup(d)
	var h := macro_h + det.height(d, profile.x, profile.y, profile.z,
		surface_composition_from_lookup(lookup), geomorph_composition_from_lookup(lookup),
		drainage_composition(d))
	h += coast_profile_offset(d, macro_h)
	# Macro rivers only cut the terrestrial surface. The old sea-level branch also
	# controlled this implicitly; keep that behaviour without changing detail amplitude.
	#
	# Faded in rather than switched on. `if w > 4.0` put a step of about eighteen
	# centimetres along the contour where the interpolated river width crosses
	# four metres -- a closed loop around every river cell on the planet. A step
	# is a discontinuity in the height field, so the mesh normals spike along it,
	# and it arrived on screen as a dotted dark hairline tracing every drainage
	# network from the air: the most conspicuous non-terrain marking on the whole
	# surface, from a term meant to be invisible.
	if macro_h > 0.0:
		var w := grid.sample_bilinear(fields.river_width, d)
		var cut := smoothstep(3.0, 16.0, w) * clampf(w * 0.045, 0.0, 22.0)
		h -= cut
	if snap.is_empty():
		h += Deltas.offset_at(d)
	else:
		h += Deltas.offset_at_snapshot(d, snap)
	return h

## Height with no player edits applied -- the "regenerate from seed" reference.
func pristine_height(d: Vector3, detail: TerrainDetail = null) -> float:
	var det := detail if detail != null else _detail_main
	var macro_h := macro_height(d)
	var relief := runtime_relief(d)
	var flat := clampf(grid.sample_bilinear(fields.floodplain, d) * 0.8
		+ grid.sample_bilinear(fields.wetland, d) * 0.5, 0.0, 1.0)
	var hardness: float = 1.7 - grid.sample_bilinear(fields.erodibility, d) * 0.55
	var profile := _detail_profile(macro_h, relief, flat, hardness)
	var lookup := surface_lookup(d)
	var h := macro_h + det.height(d, profile.x, profile.y, profile.z,
		surface_composition_from_lookup(lookup), geomorph_composition_from_lookup(lookup),
		drainage_composition(d))
	h += coast_profile_offset(d, macro_h)
	if macro_h > 0.0:
		var w := grid.sample_bilinear(fields.river_width, d)
		# Same fade as terrain_height. This one feeds the orbit elevation texture,
		# so a step here reappears as a crease in relief shading and sky occlusion
		# rather than in the mesh -- the same hairline, arriving by another route.
		h -= smoothstep(3.0, 16.0, w) * clampf(w * 0.045, 0.0, 22.0)
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
		# Ocean classification follows the same smoothed runtime macro field as
		# terrain, otherwise the water coverage cache would retain the old blocky
		# cell coastline after the terrain itself had been smoothed.
		_water_mask[c] = 1.0 if (is_lake or _macro_elev[c] < 0.0) else 0.0
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
	_cell_surface = PackedColorArray()
	_cell_surface.resize(grid.cell_count)
	_cell_geomorph = PackedColorArray()
	_cell_geomorph.resize(grid.cell_count)
	var max_accum := 1.0
	for c in grid.cell_count:
		max_accum = maxf(max_accum, fields.flow_accum[c])
	var log_max := log(max_accum)

	for c in grid.cell_count:
		var col: Color = PlanetFields.BIOME_ALBEDO[fields.biome[c]]
		var vegetation := clampf(fields.vegetation[c], 0.0, 1.0)
		# Thin soil shows the bedrock underneath, and which bedrock it is matters:
		# a basalt province and a limestone plateau are not the same colour from
		# orbit even when both are bare.
		#
		# "Thin" has to mean thin, though. Keying this to most of a metre of soil
		# made every hillside on the planet a partial rock outcrop, and since rock
		# is two to six times brighter than a canopy and nearly neutral, that one
		# threshold was enough to wash whole biomes to grey-khaki. Bedrock shows
		# through at a few centimetres, not at ninety -- and not at all where
		# something is visibly growing on top of it.
		var rockiness := clampf(1.0 - fields.soil_depth[c] / 0.30, 0.0, 1.0)
		rockiness *= 1.0 - clampf(vegetation * 1.7, 0.0, 1.0)
		var rock_col: Color = PlanetFields.ROCK_COLORS[fields.rock[c]]
		_cell_color[c] = col.lerp(rock_col, rockiness * 0.88)

		if fields.elev[c] >= 0.0:
			# Drainage is visible from space. Big catchments carry pale sorted
			# sediment; in dry country their beds stay bright long after the water
			# is gone, and they are the strongest fine texture on a desert.
			var drainage := clampf(log(maxf(fields.flow_accum[c], 1.0)) / log_max, 0.0, 1.0)
			var wash := smoothstep(0.55, 0.95, drainage) * (1.0 - clampf(fields.precip[c] / 900.0, 0.0, 1.0))
			_cell_color[c] = _cell_color[c].lerp(Color(0.330, 0.288, 0.205), wash * 0.55)
			# Valley floors that do stay wet go the other way: dark alluvium.
			var alluvium := fields.floodplain[c] * clampf(fields.soil_organic[c] * 3.4, 0.0, 1.0)
			_cell_color[c] = _cell_color[c].lerp(Color(0.075, 0.062, 0.042), alluvium * 0.42)
			# Wet ground is darker than dry ground of the same material.
			var damp := clampf(fields.soil_moisture[c], 0.0, 1.0)
			var shade := 1.0 - damp * 0.14
			_cell_color[c] = Color(_cell_color[c].r * shade, _cell_color[c].g * shade,
				_cell_color[c].b * shade)

		var wetness := clampf(fields.soil_moisture[c] * 0.78 + fields.wetland[c] * 0.55, 0.0, 1.0)
		var soil_cover := smoothstep(0.04, 0.85, maxf(0.0, fields.soil_depth[c]))
		var aeolian := fields.soil_sand[c] * (1.0 - wetness) \
			* (1.0 - vegetation * 0.62) * soil_cover
		var frost := clampf((3.5 - fields.temp_mean[c]) / 17.5, 0.0, 1.0)
		_cell_surface[c] = Color(vegetation, wetness, clampf(aeolian, 0.0, 1.0), frost)

		# Biome classification supplies the dominant surface process; continuous
		# geology and relief decide how strongly that process can sculpt the land.
		var biome: int = fields.biome[c]
		var relief_rugged := smoothstep(140.0, 1250.0, _macro_relief[c])
		var tectonic := clampf(fields.fault[c] * 0.70
			+ absf(fields.uplift[c]) / maxf(1.0, cfg.max_uplift) * 0.75, 0.0, 1.0)
		var rock_exposure := clampf(1.0 - fields.soil_depth[c] / 0.95, 0.0, 1.0)
		var mountain := maxf(relief_rugged,
			tectonic * smoothstep(450.0, 1800.0, _macro_elev[c]))
		if biome == PlanetFields.Biome.ALPINE or biome == PlanetFields.Biome.BARE_ROCK:
			mountain = maxf(mountain, 0.82 + rock_exposure * 0.18)
		elif biome in [PlanetFields.Biome.TUNDRA, PlanetFields.Biome.TAIGA]:
			mountain = maxf(mountain, relief_rugged * 0.72)

		var arid := 0.0
		match biome:
			PlanetFields.Biome.HOT_DESERT: arid = 1.0
			PlanetFields.Biome.COLD_DESERT: arid = 0.86
			PlanetFields.Biome.STEPPE: arid = 0.58
			PlanetFields.Biome.MEDITERRANEAN: arid = 0.32
			PlanetFields.Biome.SAVANNA: arid = 0.24
		arid *= 1.0 - wetness * 0.82

		var glacial := frost * 0.28
		match biome:
			PlanetFields.Biome.ICE_CAP: glacial = 1.0
			PlanetFields.Biome.TUNDRA: glacial = maxf(glacial, 0.58)
			PlanetFields.Biome.ALPINE: glacial = maxf(glacial, frost * 0.76)

		var depositional := clampf(fields.floodplain[c] * 0.78
			+ fields.wetland[c] * 0.92, 0.0, 1.0)
		if biome == PlanetFields.Biome.WETLAND:
			depositional = 1.0
		elif biome in [PlanetFields.Biome.RIVER, PlanetFields.Biome.LAKE]:
			depositional = maxf(depositional, 0.88)
		_cell_geomorph[c] = Color(clampf(mountain, 0.0, 1.0), clampf(arid, 0.0, 1.0),
			clampf(glacial, 0.0, 1.0), clampf(depositional, 0.0, 1.0))

func has_water(d: Vector3) -> bool:
	var c := grid.dir_to_index(d)
	return fields.lake_level[c] > -1e8 or macro_height(d) < 0.0

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
		"macro_elevation": macro_height(d),
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
	return surface_color_from_lookup(surface_lookup(d))

func surface_lookup(d: Vector3) -> Vector3:
	return _warp_lookup(d)

func surface_color_from_lookup(lookup: Vector3) -> Color:
	return grid.sample_color_bilinear(_cell_color, lookup)

## Continuous material weights consumed by geometry and shading.
##
## A categorical biome ID is excellent for UI labels but produces visible
## borders when used directly by a renderer. These physically meaningful fields
## interpolate across the spherical grid and share the same organic lookup warp
## as albedo: R vegetation, G wetness, B loose wind-worked sand, A frost/snow.
func surface_composition(d: Vector3) -> Color:
	return surface_composition_from_lookup(surface_lookup(d))

func surface_composition_from_lookup(lookup: Vector3) -> Color:
	return grid.sample_color_bilinear(_cell_surface, lookup)

func geomorph_composition(d: Vector3) -> Color:
	return geomorph_composition_from_lookup(surface_lookup(d))

func geomorph_composition_from_lookup(lookup: Vector3) -> Color:
	return grid.sample_color_bilinear(_cell_geomorph, lookup)
