extends "res://scripts/gen/planet_sampler.gd"
## Runtime planet sampler for the resident global terrain architecture.
##
## Height and material classification are reconstructed once from the generated
## planet, stored as whole-planet cube-face texture arrays, and loaded once on
## later starts. Normal movement therefore causes no terrain-height or material
## recenter jobs.

const SPLINE_SAMPLE_UPSAMPLE: int = 2
const SPLINE_GUTTER: int = 8
const GLOBAL_HEIGHT_FACE_RES: int = 2048
const GLOBAL_MATERIAL_FACE_RES: int = 1024
const GLOBAL_MATERIAL_GUTTER: int = 16
const EXTRA_MACRO_SMOOTH_PASSES: int = 4
const EXTRA_MACRO_SMOOTH_STRENGTH: float = 0.72

var global_height_texture: Texture2DArray
var global_height_face_res: int = 0
var global_height_cache_hit: bool = false
var global_height_cache_path: String = ""
var global_height_compressed_bytes: int = 0
var global_height_raw_bytes: int = 0
var global_height_build_ms: int = 0

var global_material_texture: Texture2DArray
var global_material_face_res: int = 0
var global_material_cache_hit: bool = false
var global_material_cache_path: String = ""
var global_material_compressed_bytes: int = 0
var global_material_raw_bytes: int = 0
var global_material_build_ms: int = 0


func _build_smoothed_macro_elevation() -> void:
	super._build_smoothed_macro_elevation()
	for _pass: int in EXTRA_MACRO_SMOOTH_PASSES:
		var src: PackedFloat32Array = _macro_elev
		var dst := PackedFloat32Array()
		dst.resize(grid.cell_count)
		for c: int in grid.cell_count:
			var base: int = c * 8
			var blurred: float = src[c] * 4.0
			blurred += src[grid.nbr[base + 0]] * 2.0
			blurred += src[grid.nbr[base + 2]] * 2.0
			blurred += src[grid.nbr[base + 4]] * 2.0
			blurred += src[grid.nbr[base + 6]] * 2.0
			blurred += src[grid.nbr[base + 1]]
			blurred += src[grid.nbr[base + 3]]
			blurred += src[grid.nbr[base + 5]]
			blurred += src[grid.nbr[base + 7]]
			blurred *= 1.0 / 16.0
			dst[c] = lerpf(src[c], blurred, EXTRA_MACRO_SMOOTH_STRENGTH)
		_macro_elev = dst
	_macro_relief = PackedFloat32Array()
	_macro_relief.resize(grid.cell_count)
	for c: int in grid.cell_count:
		var h: float = _macro_elev[c]
		var relief := 0.0
		for k: int in 8:
			relief = maxf(relief, absf(h - _macro_elev[grid.nbr[c * 8 + k]]))
		_macro_relief[c] = relief


func macro_height(d: Vector3) -> float:
	return grid.sample_cubic_bspline(_macro_elev, d)


func _global_height_variant_key() -> String:
	return str(coast_profile_points()).sha256_text().substr(0, 12)


func _build_orbit_textures() -> void:
	rebuild_global_height(false)
	rebuild_global_material(false)


func rebuild_global_height(force_rebuild: bool = false) -> bool:
	if cfg == null or grid == null:
		return false
	var started_ms: int = Time.get_ticks_msec()
	global_height_texture = null
	global_height_face_res = GLOBAL_HEIGHT_FACE_RES
	global_height_cache_hit = false
	global_height_cache_path = ""
	global_height_compressed_bytes = 0
	global_height_raw_bytes = 0
	var source_res: int = grid.res
	var spline_res: int = source_res * SPLINE_SAMPLE_UPSAMPLE
	var visual_res: int = GLOBAL_HEIGHT_FACE_RES
	var variant_key: String = _global_height_variant_key()
	var visual_gutter: int = maxi(4, int(round(
		float(SPLINE_GUTTER) * float(visual_res) / float(spline_res))))
	var spline_tex_res: int = spline_res + SPLINE_GUTTER * 2
	var visual_tex_res: int = visual_res + visual_gutter * 2
	if not force_rebuild:
		var cached: Dictionary = GlobalHeightmapCache.load_images(
			cfg, visual_res, visual_tex_res, variant_key)
		if not cached.is_empty():
			var cached_images: Array[Image] = []
			var cached_value: Variant = cached.get("images", [])
			if cached_value is Array:
				for item: Variant in cached_value:
					if item is Image:
						cached_images.append(item as Image)
			if _publish_global_height(cached_images, visual_res):
				global_height_cache_hit = true
				global_height_cache_path = String(cached["path"])
				global_height_compressed_bytes = int(cached["compressed_bytes"])
				global_height_raw_bytes = int(cached["raw_bytes"])
				global_height_build_ms = Time.get_ticks_msec() - started_ms
				return true
	var spline_step: float = 2.0 / float(spline_res)
	var images: Array[Image] = []
	for face: int in 6:
		var img := Image.create(spline_tex_res, spline_tex_res, false, Image.FORMAT_RF)
		for y: int in spline_tex_res:
			var j: int = y - SPLINE_GUTTER
			var v: float = (float(j) + 0.5) * spline_step - 1.0
			for x: int in spline_tex_res:
				var i: int = x - SPLINE_GUTTER
				var u: float = (float(i) + 0.5) * spline_step - 1.0
				var d: Vector3 = CubeSphere.face_uv_to_dir(face, u, v)
				var macro_h: float = grid.sample_cubic_bspline(_macro_elev, d)
				var h: float = macro_h + coast_profile_offset(d, macro_h)
				img.set_pixel(x, y, Color(h, 0.0, 0.0, 1.0))
		img.resize(visual_tex_res, visual_tex_res, Image.INTERPOLATE_LANCZOS)
		var mip_err: Error = img.generate_mipmaps()
		if mip_err != OK:
			push_error("Failed to generate global heightmap mipmaps (%d)" % mip_err)
		images.append(img)
	var saved: Dictionary = GlobalHeightmapCache.save_images(
		cfg, visual_res, visual_tex_res, images, variant_key)
	if not saved.is_empty():
		global_height_cache_path = String(saved["path"])
		global_height_compressed_bytes = int(saved["compressed_bytes"])
		global_height_raw_bytes = int(saved["raw_bytes"])
	var published: bool = _publish_global_height(images, visual_res)
	global_height_build_ms = Time.get_ticks_msec() - started_ms
	return published


func _publish_global_height(images: Array[Image], face_res: int) -> bool:
	if images.size() != 6:
		return false
	var texture_array := Texture2DArray.new()
	var err: Error = texture_array.create_from_images(images)
	if err != OK:
		push_error("Failed to create global height Texture2DArray (%d)" % err)
		return false
	global_height_texture = texture_array
	global_height_face_res = face_res
	orbit_elevation_texture = texture_array
	orbit_texture_face_res = face_res
	return true


func rebuild_global_material(force_rebuild: bool = false) -> bool:
	if cfg == null or grid == null or _cell_surface.is_empty():
		return false
	var started_ms: int = Time.get_ticks_msec()
	global_material_texture = null
	global_material_face_res = GLOBAL_MATERIAL_FACE_RES
	global_material_cache_hit = false
	global_material_cache_path = ""
	global_material_compressed_bytes = 0
	global_material_raw_bytes = 0
	var face_res: int = GLOBAL_MATERIAL_FACE_RES
	var gutter: int = GLOBAL_MATERIAL_GUTTER
	var tex_res: int = face_res + gutter * 2
	if not force_rebuild:
		var cached: Dictionary = GlobalMaterialMapCache.load_images(cfg, face_res, tex_res)
		if not cached.is_empty():
			var cached_images: Array[Image] = []
			var cached_value: Variant = cached.get("images", [])
			if cached_value is Array:
				for item: Variant in cached_value:
					if item is Image:
						cached_images.append(item as Image)
			if _publish_global_material(cached_images, face_res):
				global_material_cache_hit = true
				global_material_cache_path = String(cached["path"])
				global_material_compressed_bytes = int(cached["compressed_bytes"])
				global_material_raw_bytes = int(cached["raw_bytes"])
				global_material_build_ms = Time.get_ticks_msec() - started_ms
				return true
	var step: float = 2.0 / float(face_res)
	var images: Array[Image] = []
	for face: int in 6:
		var img := Image.create(tex_res, tex_res, false, Image.FORMAT_RGBA8)
		for y: int in tex_res:
			var j: int = y - gutter
			var v: float = (float(j) + 0.5) * step - 1.0
			for x: int in tex_res:
				var i: int = x - gutter
				var u: float = (float(i) + 0.5) * step - 1.0
				var d: Vector3 = CubeSphere.face_uv_to_dir(face, u, v)
				img.set_pixel(x, y, surface_composition(d))
		var mip_err: Error = img.generate_mipmaps()
		if mip_err != OK:
			push_error("Failed to generate global material mipmaps (%d)" % mip_err)
		images.append(img)
	var saved: Dictionary = GlobalMaterialMapCache.save_images(cfg, face_res, tex_res, images)
	if not saved.is_empty():
		global_material_cache_path = String(saved["path"])
		global_material_compressed_bytes = int(saved["compressed_bytes"])
		global_material_raw_bytes = int(saved["raw_bytes"])
	var published: bool = _publish_global_material(images, face_res)
	global_material_build_ms = Time.get_ticks_msec() - started_ms
	return published


func _publish_global_material(images: Array[Image], face_res: int) -> bool:
	if images.size() != 6:
		return false
	var texture_array := Texture2DArray.new()
	var err: Error = texture_array.create_from_images(images)
	if err != OK:
		push_error("Failed to create global material Texture2DArray (%d)" % err)
		return false
	global_material_texture = texture_array
	global_material_face_res = face_res
	return true


func global_height_stats() -> Dictionary:
	return {
		"resident": global_height_texture != null,
		"face_res": global_height_face_res,
		"cache_hit": global_height_cache_hit,
		"cache_path": global_height_cache_path,
		"compressed_bytes": global_height_compressed_bytes,
		"raw_bytes": global_height_raw_bytes,
		"load_or_build_ms": global_height_build_ms,
		"streaming": false,
	}


func global_material_stats() -> Dictionary:
	return {
		"resident": global_material_texture != null,
		"face_res": global_material_face_res,
		"cache_hit": global_material_cache_hit,
		"cache_path": global_material_cache_path,
		"compressed_bytes": global_material_compressed_bytes,
		"raw_bytes": global_material_raw_bytes,
		"load_or_build_ms": global_material_build_ms,
		"streaming": false,
	}
