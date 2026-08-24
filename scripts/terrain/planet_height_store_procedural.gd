extends "res://scripts/terrain/planet_height_store_playable.gd"
## CPU companion for the fully procedural GPU terrain.
##
## Visual terrain evaluates this spectrum in the vertex shader. Physics only needs
## a small local bubble, so its height pages evaluate the same inexpensive value-
## noise bands on CPU instead of invoking the old TerrainDetail synthesis. Existing
## TerrainDetail .ghz files use a different namespace and cannot leak into this path.

const PROCEDURAL_CACHE_SUFFIX := "_gpuv3"


func _configure_namespace() -> void:
	super._configure_namespace()
	_mutex.lock()
	_namespace += PROCEDURAL_CACHE_SUFFIX
	_mutex.unlock()
	var root: String = ProjectSettings.globalize_path("user://%s/%s" % [CACHE_DIR, _namespace])
	DirAccess.make_dir_recursive_absolute(root)


func _build_tile(_level: int, face: int, tile_x: int, tile_y: int,
		cells: int) -> PackedFloat32Array:
	var heights := PackedFloat32Array()
	heights.resize(TILE_VERTS * TILE_VERTS)
	var start_x: int = tile_x * TILE_CELLS
	var start_y: int = tile_y * TILE_CELLS
	var spacing: float = PI * 0.5 * Planet.cfg.planet_radius / float(cells)
	var detail_seed: int = Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff

	for y: int in TILE_VERTS:
		var gy: int = mini(start_y + y, cells)
		var v: float = -1.0 + 2.0 * float(gy) / float(cells)
		for x: int in TILE_VERTS:
			var gx: int = mini(start_x + x, cells)
			var u: float = -1.0 + 2.0 * float(gx) / float(cells)
			var d: Vector3 = CubeSphere.face_uv_to_dir(face, u, v)
			var macro_h: float = Planet.macro_height(d)
			heights[y * TILE_VERTS + x] = macro_h + _procedural_detail(
				d, spacing, macro_h, maxi(detail_seed, 1))
	return heights


func _procedural_detail(d: Vector3, spacing: float, macro_h: float,
		noise_seed: int) -> float:
	var world_m: Vector3 = d * Planet.cfg.planet_radius
	var coast_guard: float = lerpf(0.38, 1.0,
		_smootherstep(45.0, 320.0, absf(macro_h)))
	var mountain: float = _smootherstep(500.0, 2600.0, maxf(macro_h, 0.0))
	var regional_gain: float = lerpf(0.72, 1.30, mountain)
	var strength: float = maxf(0.05, Planet.cfg.detail_amplitude / 260.0)

	var h := 0.0
	h += _detail_band(world_m, 12000.0, spacing, noise_seed + 11,
		lerpf(52.0, 115.0, mountain))

	var w2400: float = _band_weight(2400.0, spacing)
	if w2400 > 0.001:
		var n: float = _smooth_value_noise(world_m / 2400.0, noise_seed + 23)
		var ridge: float = (1.0 - absf(n)) * 2.0 - 1.0
		h += (n * 28.0 + ridge * 18.0 * mountain) * w2400

	h += _detail_band(world_m, 480.0, spacing, noise_seed + 37,
		lerpf(7.0, 15.0, mountain))
	h += _detail_band(world_m, 96.0, spacing, noise_seed + 53, 3.4)
	h += _detail_band(world_m, 24.0, spacing, noise_seed + 71, 0.85)
	return h * coast_guard * regional_gain * strength


static func _smootherstep(edge0: float, edge1: float, x: float) -> float:
	var t: float = clampf((x - edge0) / maxf(edge1 - edge0, 1e-9), 0.0, 1.0)
	return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)


static func _band_weight(wavelength_m: float, spacing_m: float) -> float:
	return _smootherstep(spacing_m * 4.0, spacing_m * 8.0, wavelength_m)


func _detail_band(world_m: Vector3, wavelength_m: float, spacing_m: float,
		noise_seed: int, amplitude_m: float) -> float:
	var w: float = _band_weight(wavelength_m, spacing_m)
	if w <= 0.001:
		return 0.0
	return _smooth_value_noise(world_m / wavelength_m, noise_seed) * amplitude_m * w


static func _u32(value: int) -> int:
	return value & 0xffffffff


static func _mul_u32(a: int, b: int) -> int:
	var ua: int = _u32(a)
	var ub: int = _u32(b)
	var a0: int = ua & 0xffff
	var a1: int = (ua >> 16) & 0xffff
	var b0: int = ub & 0xffff
	var b1: int = (ub >> 16) & 0xffff
	var low: int = a0 * b0
	var mid: int = (a0 * b1 + a1 * b0) & 0xffff
	return _u32(low + (mid << 16))


static func _terrain_hash(p: Vector3i, noise_seed: int) -> int:
	var h: int = _mul_u32(p.x, 0x8da6b343)
	h = _u32(h ^ _mul_u32(p.y, 0xd8163841))
	h = _u32(h ^ _mul_u32(p.z, 0xcb1ab31f))
	h = _u32(h ^ _mul_u32(noise_seed, 0x9e3779b9))
	h = _u32(h ^ (h >> 16))
	h = _mul_u32(h, 0x7feb352d)
	h = _u32(h ^ (h >> 15))
	h = _mul_u32(h, 0x846ca68b)
	h = _u32(h ^ (h >> 16))
	return h


static func _lattice_value(p: Vector3i, noise_seed: int) -> float:
	var h: int = _terrain_hash(p, noise_seed) & 0x00ffffff
	return float(h) * (2.0 / 16777215.0) - 1.0


static func _smooth_value_noise(p: Vector3, noise_seed: int) -> float:
	var fx0: float = floor(p.x)
	var fy0: float = floor(p.y)
	var fz0: float = floor(p.z)
	var i := Vector3i(int(fx0), int(fy0), int(fz0))
	var f := Vector3(p.x - fx0, p.y - fy0, p.z - fz0)
	f = Vector3(
		f.x * f.x * f.x * (f.x * (f.x * 6.0 - 15.0) + 10.0),
		f.y * f.y * f.y * (f.y * (f.y * 6.0 - 15.0) + 10.0),
		f.z * f.z * f.z * (f.z * (f.z * 6.0 - 15.0) + 10.0))

	var n000: float = _lattice_value(i + Vector3i(0, 0, 0), noise_seed)
	var n100: float = _lattice_value(i + Vector3i(1, 0, 0), noise_seed)
	var n010: float = _lattice_value(i + Vector3i(0, 1, 0), noise_seed)
	var n110: float = _lattice_value(i + Vector3i(1, 1, 0), noise_seed)
	var n001: float = _lattice_value(i + Vector3i(0, 0, 1), noise_seed)
	var n101: float = _lattice_value(i + Vector3i(1, 0, 1), noise_seed)
	var n011: float = _lattice_value(i + Vector3i(0, 1, 1), noise_seed)
	var n111: float = _lattice_value(i + Vector3i(1, 1, 1), noise_seed)

	var nx00: float = lerpf(n000, n100, f.x)
	var nx10: float = lerpf(n010, n110, f.x)
	var nx01: float = lerpf(n001, n101, f.x)
	var nx11: float = lerpf(n011, n111, f.x)
	return lerpf(lerpf(nx00, nx10, f.y), lerpf(nx01, nx11, f.y), f.z)


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["procedural_collision"] = true
	out["terrain_detail_generator"] = false
	out["c2_noise"] = true
	return out
